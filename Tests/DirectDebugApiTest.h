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
        String successStatus;
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

    static void receiveSinkMessage(void* context, char const* receiver, char const* selector, int argc, t_atom* argv)
    {
        static_cast<DirectDebugApiTest*>(context)->captureSinkMessage(receiver, selector, argc, argv);
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

    static var makeSendObjectRequest(int const requestId, String const& generation, Array<int> const& path, var const& objectOrdinal,
        String const& selector, Array<var> const& atoms)
    {
        auto* request = new DynamicObject();
        request->setProperty("version", 1);
        request->setProperty("request_id", requestId);
        request->setProperty("operation", "send_object");
        request->setProperty("generation", generation);

        var canvasPath = Array<var>();
        for (auto const ordinal : path)
            canvasPath.getArray()->add(ordinal);
        request->setProperty("canvas_path", canvasPath);
        request->setProperty("object_ordinal", objectOrdinal);
        request->setProperty("selector", selector);
        var atomList = atoms;
        request->setProperty("atoms", atomList);
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

    static String formatSinkMessage(char const* receiver, char const* selector, int const argc, t_atom* argv)
    {
        String message = String::fromUTF8(receiver) + ":" + String::fromUTF8(selector) + "[";
        for (int index = 0; index < argc; ++index) {
            if (index != 0)
                message += ",";
            if (argv[index].a_type == A_FLOAT)
                message += String(atom_getfloat(argv + index));
            else if (argv[index].a_type == A_SYMBOL)
                message += String::fromUTF8(atom_getsymbol(argv + index)->s_name);
            else
                message += "<invalid>";
        }
        return message + "]";
    }

    void captureSinkMessage(char const* receiver, char const* selector, int const argc, t_atom* argv)
    {
        sinkMessages.add(formatSinkMessage(receiver, selector, argc, argv));
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
        rootSinkReceiver = pd::Setup::createReceiver(this, "__pd_mcp_debug_root_sink", nullptr, nullptr, nullptr, nullptr, receiveSinkMessage);
        nestedSinkReceiver = pd::Setup::createReceiver(this, "__pd_mcp_debug_nested_sink", nullptr, nullptr, nullptr, nullptr, receiveSinkMessage);
        fallbackSinkReceiver = pd::Setup::createReceiver(this, "__pd_mcp_debug_fallback_sink", nullptr, nullptr, nullptr, nullptr, receiveSinkMessage);
        editor->pd->unlockAudioThread();

        rootCanvas = editor->getTabComponent().openPatch(makeFixturePatch());
        check(rootCanvas != nullptr, "the generation fixture canvas must open");
        if (!rootCanvas) {
            finishProtocol();
            return;
        }

        rootCanvas->performSynchronise();
        originalCanvasContent = rootCanvas->patch.getCanvasContent();
        rootReceiver = String::repeatedString("r", 128);
        auto* root = rootCanvas->patch.getRawPointer();
        editor->pd->lockAudioThread();
        pd_bind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(rootReceiver));
        editor->pd->unlockAudioThread();

        sendRequest(makeSetGenerationRequest(1, "generation-1", rootReceiver), 1, true, {});
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;

        auto const generation = String("generation-1");
        Array<var> noAtoms;
        Array<var> oneFloat { 12.5 };
        Array<var> oneSymbol { "value" };
        Array<var> listAtoms { 12.5, "value" };
        auto sendObject = [this, &generation](int const requestId, Array<int> const& path, int const ordinal, String const& selector, Array<var> const& atoms) {
            sendRequest(makeSendObjectRequest(requestId, generation, path, ordinal, selector, atoms), requestId, true, {});
            expectedReplies.back().successStatus = "invoked";
        };
        auto expectSink = [this](String const& receiver, String const& selector, String const& atoms) {
            expectedSinkMessages.add(receiver + ":" + selector + "[" + atoms + "]");
        };
        sendObject(40, {}, 0, "bang", noAtoms);
        expectSink("__pd_mcp_debug_root_sink", "bang", "");
        sendObject(41, {}, 0, "float", oneFloat);
        expectSink("__pd_mcp_debug_root_sink", "float", "12.5");
        sendObject(42, {}, 0, "symbol", oneSymbol);
        expectSink("__pd_mcp_debug_root_sink", "symbol", "value");
        sendObject(43, {}, 0, "list", listAtoms);
        expectSink("__pd_mcp_debug_root_sink", "list", "12.5,value");
        sendObject(44, {}, 0, "custom", listAtoms);
        expectSink("__pd_mcp_debug_root_sink", "custom", "12.5,value");
        sendObject(45, {}, 0, "list", noAtoms);
        expectSink("__pd_mcp_debug_root_sink", "list", "");
        sendObject(46, { 3 }, 0, "bang", noAtoms);
        expectSink("__pd_mcp_debug_nested_sink", "bang", "");
        sendObject(47, { 3 }, 0, "float", oneFloat);
        expectSink("__pd_mcp_debug_nested_sink", "float", "12.5");
        sendObject(48, { 3 }, 0, "symbol", oneSymbol);
        expectSink("__pd_mcp_debug_nested_sink", "symbol", "value");
        sendObject(49, { 3 }, 0, "list", listAtoms);
        expectSink("__pd_mcp_debug_nested_sink", "list", "12.5,value");
        sendObject(50, { 3 }, 0, "custom", listAtoms);
        expectSink("__pd_mcp_debug_nested_sink", "custom", "12.5,value");
        sendObject(51, { 3 }, 0, "list", noAtoms);
        expectSink("__pd_mcp_debug_nested_sink", "list", "");
        sendRequest(makeSendObjectRequest(52, "stale-generation", {}, 0, "bang", noAtoms), 52, false, "StaleGeneration");
        sendRequest(makeSendObjectRequest(53, generation, { 1 }, 0, "bang", noAtoms), 53, false, "CanvasTypeMismatch");
        sendRequest(makeSendObjectRequest(54, generation, { 99 }, 0, "bang", noAtoms), 54, false, "CanvasNotFound");
        sendRequest(makeSendObjectRequest(55, generation, {}, 99, "bang", noAtoms), 55, false, "ObjectNotFound");
        sendRequest(makeSendObjectRequest(56, generation, {}, 0, "bang", oneFloat), 56, false, "InvalidSelector");
        sendRequest(makeSendObjectRequest(57, generation, {}, 0, "float", oneSymbol), 57, false, "InvalidAtoms");
        sendRequest(makeSendObjectRequest(58, generation, {}, static_cast<int64>(INT_MAX) + 1, "bang", noAtoms), 58, false, "InvalidRequest");
        sendRequest(makeSendObjectRequest(59, String::repeatedString("g", 129), {}, 0, "bang", noAtoms), 59, false, "InvalidRequest");
        sendRequest(makeSendObjectRequest(60, generation, {}, 0, "float", { 1.0e100 }), 60, false, "InvalidAtoms");
        sendRequest(makeSendObjectRequest(61, generation, {}, 0, "", noAtoms), 61, false, "InvalidSelector");
        sendRequest(makeSendObjectRequest(62, generation, {}, 2, "bang", noAtoms), 62, false, "ObjectTypeMismatch");
        sendObject(63, {}, 1, "custom", listAtoms);

        auto* rootObject = rootCanvas->patch.getRawPointer()->gl_list;
        editor->pd->sendDirectMessage(&rootObject->g_pd, SmallString("custom"), SmallArray<pd::Atom> { 7.0f, editor->pd->generateSymbol("direct") });
        editor->pd->sendDirectMessage(&rootObject->g_pd, SmallArray<pd::Atom> { 8.0f, editor->pd->generateSymbol("list") });
        editor->pd->sendDirectMessage(&rootObject->g_pd, SmallString("symbol-direct"));
        editor->pd->sendDirectMessage(&rootObject->g_pd, 9.0f);
        editor->pd->sendMessage("__pd_mcp_debug_fallback_sink", "fallback", { 10.0f });
        expectedSinkMessages.insert(0, "__pd_mcp_debug_root_sink:custom[7,direct]");
        expectedSinkMessages.insert(1, "__pd_mcp_debug_root_sink:list[8,list]");
        expectedSinkMessages.insert(2, "__pd_mcp_debug_root_sink:symbol[symbol-direct]");
        expectedSinkMessages.insert(3, "__pd_mcp_debug_root_sink:float[9]");
        expectedSinkMessages.insert(4, "__pd_mcp_debug_fallback_sink:fallback[10]");

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

        sendJson("{\"version\":1,\"request_id\":24,\"operation\":\"set_generation\",\"generation\":'g',\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":25,\"operation\":\"set_generation\",\"generation\":\"g\\q\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":25,\"operation\":\"set_generation\",\"generation\":\"g\\a\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":32,\"operation\":\"set_generation\",\"generation\":\"g\\u0000tail\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":26,\"operation\":\"set_generation\",\"generation\":\"g" + String::charToString('\n') + "\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":027,\"operation\":\"set_generation\",\"generation\":\"g\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":- 1,\"operation\":\"set_generation\",\"generation\":\"g\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":30.,\"operation\":\"set_generation\",\"generation\":\"g\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":31e+,\"operation\":\"set_generation\",\"generation\":\"g\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidEnvelope");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1e0,\"request_id\":28,\"operation\":\"set_generation\",\"generation\":\"g\",\"root_receiver\":\"" + rootReceiver + "\"}", 28, false, "UnsupportedProtocolVersion");
        expectedReplies.back().generationProbe = "generation-1";
        expectedReplies.back().generationActive = true;

        String validEscapedGeneration = "g\"\\/";
        for (auto const control : { '\b', '\f', '\n', '\r', '\t' })
            validEscapedGeneration += String::charToString(control);
        validEscapedGeneration += "h";
        sendJson("{\"version\":1,\"request_id\":29,\"operation\":\"set_generation\",\"generation\":\"g\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0068\",\"root_receiver\":\"" + rootReceiver + "\"}", 29, true, {});
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;

        String const oracleOverflow = "18446744073709551617";
        String const veryLongInteger = String::repeatedString("9", 4096);

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
        sendJson("{\"version\":1,\"request_id\":16777216,\"operation\":\"set_generation\",\"generation\":\"overflow-id\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidRequest");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":" + oracleOverflow + ",\"operation\":\"set_generation\",\"generation\":\"overflow-id\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidRequest");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":-" + oracleOverflow + ",\"operation\":\"set_generation\",\"generation\":\"overflow-id\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidRequest");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":" + veryLongInteger + ",\"operation\":\"set_generation\",\"generation\":\"overflow-id\",\"root_receiver\":\"" + rootReceiver + "\"}", 0, false, "InvalidRequest");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":1,\"request_id\":true,\"operation\":\"set_generation\"}", 0, false, "InvalidRequest");

        sendJson("{\"version\":2,\"request_id\":7,\"operation\":\"set_generation\"}", 7, false, "UnsupportedProtocolVersion");
        sendJson("{\"version\":" + oracleOverflow + ",\"request_id\":33,\"operation\":\"set_generation\",\"generation\":\"overflow-version\",\"root_receiver\":\"" + rootReceiver + "\"}", 33, false, "UnsupportedProtocolVersion");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":-" + oracleOverflow + ",\"request_id\":34,\"operation\":\"set_generation\",\"generation\":\"overflow-version\",\"root_receiver\":\"" + rootReceiver + "\"}", 34, false, "UnsupportedProtocolVersion");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"version\":" + veryLongInteger + ",\"request_id\":35,\"operation\":\"set_generation\",\"generation\":\"overflow-version\",\"root_receiver\":\"" + rootReceiver + "\"}", 35, false, "UnsupportedProtocolVersion");
        expectedReplies.back().generationProbe = validEscapedGeneration;
        expectedReplies.back().generationActive = true;
        sendJson("{\"request_id\":8,\"operation\":\"set_generation\"}", 8, false, "UnsupportedProtocolVersion");
        sendJson("{\"version\":1,\"request_id\":9,\"operation\":\"unknown\"}", 9, false, "UnknownOperation");
        sendJson("{\"version\":2,\"request_id\":10,\"operation\":\"unknown\",\"extra\":true}", 10, false, "UnsupportedProtocolVersion");
        sendJson("{\"version\":1,\"request_id\":11,\"operation\":\"unknown\",\"extra\":true}", 11, false, "UnknownOperation");

        sendRequest(makeSetGenerationRequest(12, "g", rootReceiver, true), 12, false, "InvalidRequest");
        expectedReplies.back().generationProbe = validEscapedGeneration;
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
        lifetimeGeneration = boundaryGeneration;

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
            startTimer(100);
    }

    void timerCallback() override
    {
        stopTimer();
        if (lifetimeStage == 0) {
            String actualSinkMessages;
            for (auto const& message : sinkMessages)
                actualSinkMessages << message << "|";
            check(sinkMessages.size() == expectedSinkMessages.size() && std::ranges::is_permutation(sinkMessages, expectedSinkMessages),
                "root and nested dispatch must produce exact sink output for every message form; actual=" + actualSinkMessages);
            check(rootCanvas && rootCanvas->patch.getCanvasContent() == originalCanvasContent,
                "object dispatch must not change serialized patch content while the patch exists");
            deleteRootAndProbe();
        } else if (lifetimeStage == 1) {
            probeDeletedRoot();
        } else if (lifetimeStage == 2) {
            rebuildRootAndProbeStaleGeneration();
        } else if (lifetimeStage == 3) {
            reregisterRootAndProbe();
        } else {
            finishProtocol();
        }
    }

    static String makeFixturePatch()
    {
        return "#N canvas 100 100 300 200 12;\n#X obj 20 20 s __pd_mcp_debug_root_sink;\n#X obj 20 50 print direct-root;\n#X text 20 80 comment;\n#N canvas 0 0 300 200 nested 0;\n#X obj 20 20 s __pd_mcp_debug_nested_sink;\n#X obj 20 50 print direct-nested;\n#X restore 100 100 pd nested;\n";
    }

    void deleteRootAndProbe()
    {
        auto& tabbar = editor->getTabComponent();
        tabbar.closeTab(rootCanvas);
        rootCanvas = nullptr;
        lifetimeStage = 1;
        startTimer(100);
    }

    void probeDeletedRoot()
    {
        lifetimeStage = 2;
        sendRequest(makeSendObjectRequest(70, lifetimeGeneration, {}, 0, "bang", {}), 70, false, "StaleGeneration");
        startTimer(5000);
    }

    void rebuildRootAndProbeStaleGeneration()
    {
        rootCanvas = editor->getTabComponent().openPatch(makeFixturePatch());
        check(rootCanvas != nullptr, "the deleted root must be rebuildable under the same receiver");
        if (rootCanvas) {
            rootCanvas->performSynchronise();
            auto* root = rootCanvas->patch.getRawPointer();
            editor->pd->lockAudioThread();
            pd_bind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(rootReceiver));
            editor->pd->unlockAudioThread();
        }
        lifetimeStage = 3;
        sendRequest(makeSendObjectRequest(71, lifetimeGeneration, {}, 0, "bang", {}), 71, false, "StaleGeneration");
        startTimer(5000);
    }

    void reregisterRootAndProbe()
    {
        lifetimeStage = 4;
        sendRequest(makeSetGenerationRequest(72, "generation-2", rootReceiver), 72, true, {});
        sendRequest(makeSendObjectRequest(73, "generation-2", {}, 0, "bang", {}), 73, true, {});
        expectedReplies.back().successStatus = "invoked";
        expectedSinkMessages.add("__pd_mcp_debug_root_sink:bang[]");
        startTimer(5000);
    }

    void finishProtocol()
    {
        if (finished)
            return;
        finished = true;
        stopTimer();

        check(responses.size() == static_cast<int>(expectedReplies.size()), "every request must produce exactly one response");
        check(sinkMessages.size() == expectedSinkMessages.size() && std::ranges::is_permutation(sinkMessages, expectedSinkMessages),
            "re-registration must restore successful exact sink delivery");
        String consoleText;
        for (auto const& [object, message, type, length, repeats] : editor->pd->getConsoleMessages()) {
            ignoreUnused(object, type, length, repeats);
            consoleText << message << "\n";
        }
        check(consoleText.contains("direct-root:"), "root sink receives direct and object dispatch output");
        check(consoleText.contains("direct-nested:"), "nested sink receives object dispatch output");
        check(consoleText.contains("custom"), "typed selector reaches a sink");
        check(consoleText.contains("12.5"), "numeric/list atoms reach a sink");
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
            check(ok.isBool() && static_cast<bool>(ok) == expected.ok, "response ok must match the outcome for request " + String(expected.requestId));
            check(response->getProperties().size() == 4, "responses must contain exactly four top-level fields");

            if (expected.ok) {
                check(response->hasProperty("data") && response->getProperty("data").isObject(), "successful responses must contain object data");
                if (expected.successStatus.isNotEmpty()) {
                    auto* data = response->getProperty("data").getDynamicObject();
                    check(data && data->getProperty("status").toString() == expected.successStatus,
                        "object dispatch must acknowledge invocation without semantic success claims");
                }
            } else {
                auto* error = response->getProperty("error").getDynamicObject();
                check(error != nullptr, "failed responses must contain an error object");
                if (error) {
                    check(error->getProperties().size() == 2, "errors must contain exactly code and message");
                    check(error->getProperty("code").isString() && error->getProperty("code").toString() == expected.errorCode,
                        "failed responses must use the expected stable code for request " + String(expected.requestId)
                            + "; actual=" + error->getProperty("code").toString());
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
        if (rootSinkReceiver)
            pd_free(static_cast<t_pd*>(rootSinkReceiver));
        if (nestedSinkReceiver)
            pd_free(static_cast<t_pd*>(nestedSinkReceiver));
        if (fallbackSinkReceiver)
            pd_free(static_cast<t_pd*>(fallbackSinkReceiver));

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
    void* rootSinkReceiver = nullptr;
    void* nestedSinkReceiver = nullptr;
    void* fallbackSinkReceiver = nullptr;
    String rootReceiver;
    String lifetimeGeneration;
    String originalCanvasContent;
    Array<String> sinkMessages;
    Array<String> expectedSinkMessages;
    int receivedLifecycleMessages = 0;
    int lifetimeStage = 0;
    bool allPassed = true;
    bool finished = false;
};
