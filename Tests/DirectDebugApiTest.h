#include "Pd/Setup.h"

class DirectDebugApiTest : public PlugDataUnitTest, private Timer
{
public:
    DirectDebugApiTest(PluginEditor* editor) : PlugDataUnitTest(editor, "Direct Debug API Test")
    {
    }

private:
    struct ExpectedReply {
        int requestId;
        bool ok;
        String errorCode;
        String generationProbe;
        bool generationActive = false;
    };

    static constexpr int maxEncodedRequestBytes = 64 * 1024;
    static constexpr int maxDecodedRequestBytes = 48 * 1024;
    static constexpr int maxEncodedResponseBytes = 60 * 1024;

    static void receiveLifecycleMessage(void* context, char const*, char const*, int, t_atom*)
    {
        static_cast<DirectDebugApiTest*>(context)->receivedLifecycleMessages++;
    }

    static void receiveReply(void* context, char const*, char const* selector, int argc, t_atom* argv)
    {
        static_cast<DirectDebugApiTest*>(context)->captureReply(selector, argc, argv);
    }

    void check(bool const condition, String const& description)
    {
        expect(condition, description);
        allPassed = allPassed && condition;
    }

    static var makeSetGenerationRequest(var const& requestId, var const& generation, var const& rootReceiver, bool const addExtraField = false)
    {
        auto* request = new DynamicObject();
        request->setProperty("version", 1);
        request->setProperty("request_id", requestId);
        request->setProperty("operation", "set_generation");
        request->setProperty("generation", generation);
        request->setProperty("root_receiver", rootReceiver);
        if (addExtraField)
            request->setProperty("extra", true);
        return var(request);
    }

    void sendWire(String const& selector, SmallArray<pd::Atom> const& atoms, int const requestId, bool const ok, String const& errorCode)
    {
        expectedReplies.push_back({ requestId, ok, errorCode });
        editor->pd->lockAudioThread();
        editor->pd->sendMessage("__pd_mcp_debug", selector.toRawUTF8(), atoms);
        editor->pd->unlockAudioThread();
    }

    void sendEncoded(String const& encoded, int const requestId, bool const ok, String const& errorCode)
    {
        expectedReplies.push_back({ requestId, ok, errorCode });
        editor->pd->lockAudioThread();
        editor->pd->sendMessage("__pd_mcp_debug", "request", { editor->pd->generateSymbol(encoded) });
        editor->pd->unlockAudioThread();
    }

    void sendJson(String const& json, int const requestId, bool const ok, String const& errorCode)
    {
        sendEncoded(Base64::toBase64(json.toRawUTF8(), json.getNumBytesAsUTF8()), requestId, ok, errorCode);
    }

    void sendRequest(var const& request, int const requestId, bool const ok, String const& errorCode)
    {
        sendJson(JSON::toString(request, true), requestId, ok, errorCode);
    }

    void testReceiverLifecycle()
    {
        beginTest("Reserved receiver lifecycle");

        auto createReceiver = [this] {
            return pd::Setup::createReceiver(this, "__pd_mcp_debug_lifecycle_test", nullptr, nullptr, nullptr, nullptr, receiveLifecycleMessage);
        };

        editor->pd->lockAudioThread();
        auto* receiver = createReceiver();
        editor->pd->sendMessage("__pd_mcp_debug_lifecycle_test", "probe", {});
        check(receivedLifecycleMessages == 1, "a bound receiver must receive one message");
        pd_free(static_cast<t_pd*>(receiver));

        editor->pd->sendMessage("__pd_mcp_debug_lifecycle_test", "probe", {});
        check(receivedLifecycleMessages == 1, "a freed receiver must be unbound");

        receiver = createReceiver();
        editor->pd->sendMessage("__pd_mcp_debug_lifecycle_test", "probe", {});
        check(receivedLifecycleMessages == 2, "a recreated receiver must receive one message");
        pd_free(static_cast<t_pd*>(receiver));
        editor->pd->unlockAudioThread();
    }

    void testProtocol()
    {
        beginTest("Versioned receiver and generation registration");

        editor->pd->lockAudioThread();
        replyReceiver = pd::Setup::createReceiver(this, "__pd_mcp_debug_reply", nullptr, nullptr, nullptr, nullptr, receiveReply);
        nonCanvasReceiver = pd::Setup::createReceiver(this, "__pd_mcp_debug_non_canvas", nullptr, nullptr, nullptr, nullptr, nullptr);
        editor->pd->unlockAudioThread();

        rootCanvas = editor->getTabComponent().openPatch("#N canvas 100 100 300 200 12;\n");
        check(rootCanvas != nullptr, "the generation fixture canvas must open");
        if (!rootCanvas) {
            finishProtocol();
            return;
        }

        rootCanvas->performSynchronise();
        rootReceiver = String::repeatedString("r", 128);
        auto* root = rootCanvas->patch.getRawPointer();
        editor->pd->lockAudioThread();
        pd_bind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(rootReceiver));
        editor->pd->unlockAudioThread();

        sendRequest(makeSetGenerationRequest(1, "generation-1", rootReceiver), 1, true, {});
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;

        editor->pd->lockAudioThread();
        expectedReplies.push_back({ 0, false, "InvalidEnvelope" });
        editor->pd->sendBang("__pd_mcp_debug");
        expectedReplies.push_back({ 0, false, "InvalidEnvelope" });
        editor->pd->sendFloat("__pd_mcp_debug", 1.0f);
        expectedReplies.push_back({ 0, false, "InvalidEnvelope" });
        editor->pd->sendSymbol("__pd_mcp_debug", "not-a-request");
        editor->pd->unlockAudioThread();
        sendWire("wrong", {}, 0, false, "InvalidEnvelope");
        sendWire("request", {}, 0, false, "InvalidEnvelope");
        expectedReplies.push_back({ 0, false, "InvalidEnvelope" });
        editor->pd->lockAudioThread();
        editor->pd->sendMessage("__pd_mcp_debug", "request", { editor->pd->generateSymbol("one"), editor->pd->generateSymbol("two") });
        editor->pd->unlockAudioThread();
        sendWire("request", { 1.0f }, 0, false, "InvalidEnvelope");

        sendEncoded("%%%", 0, false, "InvalidEnvelope");
        sendEncoded("AA=A", 0, false, "InvalidEnvelope");
        sendJson("{invalid", 0, false, "InvalidEnvelope");
        sendJson("{\"array\":[}]", 0, false, "InvalidEnvelope");
        sendJson("[]", 0, false, "InvalidEnvelope");
        sendJson(JSON::toString(makeSetGenerationRequest(21, "generation-trailing", rootReceiver), true) + " trailing", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("[] trailing", 0, false, "InvalidEnvelope");
        sendJson(JSON::toString(makeSetGenerationRequest(22, "generation-1", rootReceiver), true) + " \n\t", 22, true, {});
        sendJson("{\"version\":1,\"request_id\":23,\"operation\":\"unknown\",\"padding\":\"}]\"}", 23, false, "UnknownOperation");

        String const largePrefix = "{\"version\":1,\"request_id\":20,\"operation\":\"unknown\",\"padding\":\"";
        String const largeSuffix = "\"}";
        auto const paddingBytes = maxDecodedRequestBytes - largePrefix.getNumBytesAsUTF8() - largeSuffix.getNumBytesAsUTF8();
        String const maximumDecodedJson = largePrefix + String::repeatedString("x", paddingBytes) + largeSuffix;
        check(maximumDecodedJson.getNumBytesAsUTF8() == maxDecodedRequestBytes, "the decoded boundary fixture must be exactly 48 KiB");
        check(Base64::toBase64(maximumDecodedJson.toRawUTF8(), maximumDecodedJson.getNumBytesAsUTF8()).getNumBytesAsUTF8() == maxEncodedRequestBytes,
            "the encoded boundary fixture must be exactly 64 KiB");
        sendJson(maximumDecodedJson, 20, false, "UnknownOperation");
        sendJson(maximumDecodedJson + "x", 0, false, "PayloadTooLarge");

        sendJson("{\"version\":1,\"operation\":\"set_generation\",\"generation\":\"g\",\"root_receiver\":\"r\"}", 0, false, "InvalidRequest");
        sendJson("{\"version\":1,\"request_id\":1.5,\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");
        sendJson("{\"version\":1,\"request_id\":\"1\",\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");
        sendJson("{\"version\":1,\"request_id\":0,\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");
        sendJson("{\"version\":1,\"request_id\":-1,\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");
        sendJson("{\"version\":1,\"request_id\":16777216,\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");
        sendJson("{\"version\":1,\"request_id\":true,\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");

        sendJson("{\"version\":2,\"request_id\":7,\"operation\":\"set_generation\"}", 7, false, "UnsupportedProtocolVersion");
        sendJson("{\"request_id\":8,\"operation\":\"set_generation\"}", 8, false, "UnsupportedProtocolVersion");
        sendJson("{\"version\":1,\"request_id\":9,\"operation\":\"unknown\"}", 9, false, "UnknownOperation");
        sendJson("{\"version\":2,\"request_id\":10,\"operation\":\"unknown\",\"extra\":true}", 10, false, "UnsupportedProtocolVersion");
        sendJson("{\"version\":1,\"request_id\":11,\"operation\":\"unknown\",\"extra\":true}", 11, false, "UnknownOperation");

        sendRequest(makeSetGenerationRequest(12, "g", rootReceiver, true), 12, false, "InvalidRequest");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = false;
        sendJson("{\"version\":1,\"request_id\":13,\"operation\":\"set_generation\",\"root_receiver\":\"r\"}", 13, false, "InvalidRequest");
        sendRequest(makeSetGenerationRequest(14, true, rootReceiver), 14, false, "InvalidRequest");
        sendRequest(makeSetGenerationRequest(15, "", rootReceiver), 15, false, "InvalidRequest");
        sendRequest(makeSetGenerationRequest(16, String::repeatedString(String::charToString(0x00e9), 65), rootReceiver), 16, false, "InvalidRequest");
        sendRequest(makeSetGenerationRequest(17, "g", rootReceiver + "r"), 17, false, "InvalidRequest");
        sendRequest(makeSetGenerationRequest(18, "g", 42), 18, false, "InvalidRequest");

        sendRequest(makeSetGenerationRequest(19, "g", "__pd_mcp_debug_missing_root"), 19, false, "CanvasNotFound");
        sendRequest(makeSetGenerationRequest(20, "g", "__pd_mcp_debug_non_canvas"), 20, false, "CanvasTypeMismatch");
        String const boundaryGeneration = String::repeatedString(String::charToString(0x00e9), 64);
        sendRequest(makeSetGenerationRequest(16777215, boundaryGeneration, rootReceiver), 16777215, true, {});
        expectedReplies.back().generationProbe = boundaryGeneration;
        expectedReplies.back().generationActive = true;

        startTimer(5000);
    }

    void captureReply(char const* selector, int const argc, t_atom* argv)
    {
        auto const responseIndex = responses.size();
        if (responseIndex < static_cast<int>(expectedReplies.size())) {
            auto const& expected = expectedReplies[static_cast<size_t>(responseIndex)];
            if (expected.generationProbe.isNotEmpty())
                check(editor->pd->isDebugGenerationActiveUnderLock(expected.generationProbe) == expected.generationActive,
                    "generation state must reflect registration success or invalidation before the reply is observed");
        }

        var response;
        bool const validWire = String::fromUTF8(selector) == "response" && argc == 1 && argv[0].a_type == A_SYMBOL;
        check(validWire, "each reply must be selector response plus one symbol atom");

        if (validWire) {
            String const encoded = String::fromUTF8(atom_getsymbol(argv)->s_name);
            check(encoded.getNumBytesAsUTF8() <= maxEncodedResponseBytes, "encoded responses must not exceed 60 KiB");

            MemoryOutputStream decoded;
            bool const decodedOk = Base64::convertFromBase64(decoded, encoded);
            check(decodedOk, "response payloads must be valid base64");
            check(decodedOk && Base64::toBase64(decoded.getData(), decoded.getDataSize()) == encoded, "response base64 must be canonical");

            if (decodedOk) {
                String const json = String::fromUTF8(static_cast<char const*>(decoded.getData()), static_cast<int>(decoded.getDataSize()));
                auto const parseResult = JSON::parse(json, response);
                check(parseResult.wasOk() && response.isObject(), "response payloads must contain a JSON object");
                check(!json.containsIgnoreCase("pointer") && !json.containsIgnoreCase("address") && !json.contains("0x"),
                    "responses must not expose pointer-like values");
            }
        }

        responses.add(response);
        if (responses.size() >= static_cast<int>(expectedReplies.size()))
            startTimer(1);
    }

    void timerCallback() override
    {
        stopTimer();
        finishProtocol();
    }

    void finishProtocol()
    {
        if (finished)
            return;
        finished = true;
        stopTimer();

        check(responses.size() == static_cast<int>(expectedReplies.size()), "every request must produce exactly one response");
        int const responseCount = jmin(responses.size(), static_cast<int>(expectedReplies.size()));
        for (int i = 0; i < responseCount; ++i) {
            auto* response = responses[i].getDynamicObject();
            check(response != nullptr, "each decoded response must be an object");
            if (!response)
                continue;

            auto const& expected = expectedReplies[static_cast<size_t>(i)];
            auto const version = response->getProperty("version");
            auto const requestId = response->getProperty("request_id");
            auto const ok = response->getProperty("ok");
            check((version.isInt() || version.isInt64()) && static_cast<int>(version) == 1, "response version must be 1");
            check((requestId.isInt() || requestId.isInt64()) && static_cast<int>(requestId) == expected.requestId, "response IDs must preserve valid request correlation");
            check(ok.isBool() && static_cast<bool>(ok) == expected.ok, "response ok must match the outcome");
            check(response->getProperties().size() == 4, "responses must contain exactly four top-level fields");

            if (expected.ok) {
                check(response->hasProperty("data") && response->getProperty("data").isObject(), "successful responses must contain object data");
            } else {
                auto* error = response->getProperty("error").getDynamicObject();
                check(error != nullptr, "failed responses must contain an error object");
                if (error) {
                    check(error->getProperties().size() == 2, "errors must contain exactly code and message");
                    check(error->getProperty("code").isString() && error->getProperty("code").toString() == expected.errorCode,
                        "failed responses must use the expected stable code");
                    check(error->getProperty("message").isString() && error->getProperty("message").toString().isNotEmpty(),
                        "failed responses must include a non-empty message");
                }
            }
        }

        editor->pd->lockAudioThread();
        if (replyReceiver)
            pd_free(static_cast<t_pd*>(replyReceiver));
        if (nonCanvasReceiver)
            pd_free(static_cast<t_pd*>(nonCanvasReceiver));

        if (rootCanvas) {
            if (auto* root = rootCanvas->patch.getRawPointer()) {
                pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(rootReceiver));
            }
        }
        editor->pd->unlockAudioThread();

        auto& tabbar = editor->getTabComponent();
        while (auto* canvas = tabbar.getCurrentCanvas())
            tabbar.closeTab(canvas);

        signalDone(allPassed);
    }

    void perform() override
    {
        testReceiverLifecycle();
        testProtocol();
    }

    std::vector<ExpectedReply> expectedReplies;
    Array<var> responses;
    Canvas* rootCanvas = nullptr;
    void* replyReceiver = nullptr;
    void* nonCanvasReceiver = nullptr;
    String rootReceiver;
    int receivedLifecycleMessages = 0;
    bool allPassed = true;
    bool finished = false;
};
