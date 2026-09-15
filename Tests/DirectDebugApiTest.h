#include "Pd/Setup.h"
#include "Sidebar/Console.h"
#include <atomic>
#include <iostream>
#include <thread>

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

    static void receivePrintMessage(void* context, void*, char const* fragment)
    {
        auto* test = static_cast<DirectDebugApiTest*>(context);
        test->printBuffer += String::fromUTF8(fragment);
        while (test->printBuffer.containsChar('\n')) {
            test->printLines.add(test->printBuffer.upToFirstOccurrenceOf("\n", false, false));
            test->printBuffer = test->printBuffer.fromFirstOccurrenceOf("\n", false, false);
        }
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

    static var makeGetConsoleRequest(int const requestId, bool const includeHistory, int const maxEntries)
    {
        auto* request = new DynamicObject();
        request->setProperty("version", 1);
        request->setProperty("request_id", requestId);
        request->setProperty("operation", "get_console");
        request->setProperty("include_history", includeHistory);
        request->setProperty("max_entries", maxEntries);
        return var(request);
    }

    static var makeGetConsoleRequest(int const requestId, bool const includeHistory, int const maxEntries, var const& sinceId)
    {
        auto request = makeGetConsoleRequest(requestId, includeHistory, maxEntries);
        request.getDynamicObject()->setProperty("since_id", sinceId);
        return request;
    }

    // Builds a get_console request on the raw JSON wire so malformed since_id
    // values and missing mandatory fields can be exercised verbatim.
    static String makeRawGetConsoleRequest(int const requestId, String const& includeHistoryJson, String const& maxEntriesField, String const& sinceIdField)
    {
        String json = "{\"version\":1,\"request_id\":" + String(requestId)
            + ",\"operation\":\"get_console\",\"include_history\":" + includeHistoryJson;
        if (maxEntriesField.isNotEmpty())
            json += ",\"max_entries\":" + maxEntriesField;
        if (sinceIdField.isNotEmpty())
            json += ",\"since_id\":" + sinceIdField;
        return json + "}";
    }

    static var makeClearConsoleRequest(int const requestId)
    {
        auto* request = new DynamicObject();
        request->setProperty("version", 1);
        request->setProperty("request_id", requestId);
        request->setProperty("operation", "clear_console");
        return var(request);
    }

    static var makeExportCanvasRequest(int const requestId, String const& generation, var const& path, bool const addExtraField = false)
    {
        auto* request = new DynamicObject();
        request->setProperty("version", 1);
        request->setProperty("request_id", requestId);
        request->setProperty("operation", "export_canvas");
        request->setProperty("generation", generation);
        request->setProperty("path", path);
        if (addExtraField)
            request->setProperty("extra", true);
        return var(request);
    }

    static String makeExportFixturePatch()
    {
        // A single object placed well inside the visible viewport so the +24 px
        // content margin stays fully visible (clipped: false).
        return "#N canvas 100 100 300 200 12;\n#X obj 100 100 print export-target;\n";
    }

    // Mirrors the implementation's content-region computation: union of the
    // object bounds expanded by 24 px, mapped through the live canvas
    // transform and clipped to the editor.
    Rectangle<int> exportExpectedRegion(Canvas* canvas) const
    {
        Rectangle<int> contentBounds;
        bool hasContent = false;
        for (auto const* object : canvas->objects) {
            auto const bounds = object->getBounds();
            contentBounds = hasContent ? contentBounds.getUnion(bounds) : bounds;
            hasContent = true;
        }
        if (!hasContent)
            return {};

        contentBounds = contentBounds.expanded(24);
        auto const topLeft = editor->getLocalPoint(canvas, contentBounds.getTopLeft());
        auto const bottomRight = editor->getLocalPoint(canvas, contentBounds.getBottomRight());
        auto const mapped = Rectangle<int>::leftTopRightBottom(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
        return mapped.getIntersection(editor->getLocalBounds());
    }

    void checkExportSaved(int const responseIndex, bool const expectedClipped)
    {
        check(responseIndex < responses.size(), "export reply must be received");
        if (responseIndex >= responses.size())
            return;
        auto* response = responses[responseIndex].getDynamicObject();
        check(response != nullptr, "export reply must be an object");
        if (!response)
            return;
        check(static_cast<bool>(response->getProperty("ok")), "export reply ok must be true");
        auto* data = response->getProperty("data").getDynamicObject();
        check(data != nullptr, "export success must carry a data object");
        if (!data)
            return;
        check(data->getProperties().size() == 2, "export success data must contain exactly status and clipped");
        check(data->getProperty("status").isString() && data->getProperty("status").toString() == "saved",
            "export success status must be saved");
        check(data->getProperty("clipped").isBool() && static_cast<bool>(data->getProperty("clipped")) == expectedClipped,
            "export clipped flag must match");
    }

    void checkExportFile(File const& file, Rectangle<int> const& expectedLogical, String const& description)
    {
        check(file.existsAsFile(), description + " must create the requested file");
        check(file.getSize() > 0, description + " file must be non-empty");
        if (!file.existsAsFile())
            return;

        unsigned char const expectedMagic[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        unsigned char magic[8] = {};
        FileInputStream magicStream(file);
        int const magicBytes = magicStream.read(magic, 8);
        bool magicOk = magicBytes == 8;
        for (int i = 0; i < 8 && magicOk; ++i)
            magicOk = magic[i] == expectedMagic[i];
        check(magicOk, description + " file must start with the PNG magic bytes");

        FileInputStream decodeStream(file);
        Image const decoded = PNGImageFormat().decodeImage(decodeStream);
        check(!decoded.isNull(), description + " file must decode as PNG");
        if (decoded.isNull())
            return;

        auto const renderScale = editor->getRenderScale();
        int const expectedWidth = roundToInt(static_cast<float>(expectedLogical.getWidth()) * renderScale);
        int const expectedHeight = roundToInt(static_cast<float>(expectedLogical.getHeight()) * renderScale);
        check(decoded.getWidth() == expectedWidth && decoded.getHeight() == expectedHeight,
            description + " png dimensions must match the visible content region scaled by the render scale (expected "
                + String(expectedWidth) + "x" + String(expectedHeight) + ", actual " + String(decoded.getWidth()) + "x"
                + String(decoded.getHeight()) + ")");
    }

    void consoleBegin()
    {
        responses.clear();
        expectedReplies.clear();
    }

    void flushDebugQueue()
    {
        editor->pd->handleAsyncUpdate();
    }

    void consoleInject(String const& text, int const severity)
    {
        if (severity == 0)
            editor->pd->logMessage(text);
        else if (severity == 1)
            editor->pd->logWarning(text);
        else
            editor->pd->logError(text);
    }

    void checkConsoleReply(int const responseIndex, int const expectedId, bool const ok, String const& errorCode)
    {
        check(responseIndex < responses.size(), "console reply must be received");
        if (responseIndex >= responses.size())
            return;
        auto* response = responses[responseIndex].getDynamicObject();
        check(response != nullptr, "console reply must be an object");
        if (!response)
            return;
        check(static_cast<int>(response->getProperty("request_id")) == expectedId, "console reply request_id must correlate");
        check(static_cast<bool>(response->getProperty("ok")) == ok, "console reply ok must match for request " + String(expectedId));
        if (!ok) {
            auto* error = response->getProperty("error").getDynamicObject();
            check(error != nullptr, "console error reply must contain an error object");
            if (error)
                check(error->getProperty("code").isString() && error->getProperty("code").toString() == errorCode,
                    "console error code must match for request " + String(expectedId));
        }
    }

    Array<var> const* consoleEntries(int const responseIndex) const
    {
        if (responseIndex >= responses.size())
            return nullptr;
        auto* response = responses[responseIndex].getDynamicObject();
        if (!response)
            return nullptr;
        auto* data = response->getProperty("data").getDynamicObject();
        if (!data)
            return nullptr;
        return data->getProperty("entries").getArray();
    }

    bool consoleTruncated(int const responseIndex) const
    {
        if (responseIndex >= responses.size())
            return false;
        auto* response = responses[responseIndex].getDynamicObject();
        if (!response)
            return false;
        auto* data = response->getProperty("data").getDynamicObject();
        if (!data)
            return false;
        return static_cast<bool>(data->getProperty("truncated"));
    }

    static bool readConsoleCursor(var const& response, int64& cursor)
    {
        auto* obj = response.getDynamicObject();
        if (!obj)
            return false;
        auto* data = obj->getProperty("data").getDynamicObject();
        if (!data)
            return false;
        auto const value = data->getProperty("cursor");
        if (!(value.isInt() || value.isInt64()))
            return false;
        cursor = static_cast<int64>(value);
        return true;
    }

    bool consoleCursorAt(int const responseIndex, int64& cursor) const
    {
        if (responseIndex >= responses.size())
            return false;
        return readConsoleCursor(responses[responseIndex], cursor);
    }

    String consoleErrorMessage(int const responseIndex) const
    {
        if (responseIndex >= responses.size())
            return {};
        auto* response = responses[responseIndex].getDynamicObject();
        if (!response)
            return {};
        auto* error = response->getProperty("error").getDynamicObject();
        return error ? error->getProperty("message").toString() : String();
    }

    void checkConsoleErrorMessage(int const responseIndex, String const& expectedMessage)
    {
        check(responseIndex < responses.size(), "console error reply must be received");
        if (responseIndex >= responses.size())
            return;
        check(consoleErrorMessage(responseIndex) == expectedMessage,
            "console error message must match for response " + String(responseIndex));
    }

    void checkConsoleEntry(var const& entry, String const& text, String const& severity, int const repeats, String const& source, String const& description)
    {
        auto* obj = entry.getDynamicObject();
        check(obj != nullptr, description + " entry must be an object");
        if (!obj)
            return;
        check(obj->getProperty("text").isString() && obj->getProperty("text").toString() == text, description + " text must match");
        check(obj->getProperty("severity").isString() && obj->getProperty("severity").toString() == severity, description + " severity must match");
        check(obj->getProperty("repeats").isInt() && static_cast<int>(obj->getProperty("repeats")) == repeats, description + " repeats must match");
        check(obj->getProperty("source").isString() && obj->getProperty("source").toString() == source, description + " source must match");
    }

    static bool consoleEntryId(var const& entry, int64& id)
    {
        auto* obj = entry.getDynamicObject();
        if (!obj)
            return false;
        auto const value = obj->getProperty("id");
        if (!(value.isInt() || value.isInt64()))
            return false;
        id = static_cast<int64>(value);
        return true;
    }

    void checkConsoleEntryId(var const& entry, int64 const expectedId, String const& description)
    {
        int64 actualId = 0;
        check(consoleEntryId(entry, actualId), description + " must carry an id");
        check(actualId == expectedId, description + " id must match");
    }

    void checkConsoleIdsIncreasing(Array<var> const* entries, String const& description)
    {
        if (entries == nullptr) {
            check(false, description + " entries must be present");
            return;
        }

        int64 previous = 0;
        bool ok = true;
        for (auto const& entry : *entries) {
            int64 id = 0;
            if (!consoleEntryId(entry, id) || id <= 0 || id <= previous) {
                ok = false;
                break;
            }
            previous = id;
        }
        check(ok, description + " ids must be present, positive, and strictly increasing");
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
        pd_unbind(static_cast<t_pd*>(editor->pd->printReceiver), gensym("#plugdata_print"));
        printHook = pd::Setup::createPrintHook(this, receivePrintMessage);
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
        auto expectPrint = [this](String const& prefix, String const& message) {
            expectedPrintLines.add(prefix + ": " + message);
        };
        sendObject(40, {}, 0, "bang", noAtoms);
        expectPrint("direct-root", "bang");
        sendObject(41, {}, 0, "float", oneFloat);
        expectPrint("direct-root", "12.5");
        sendObject(42, {}, 0, "symbol", oneSymbol);
        expectPrint("direct-root", "symbol value");
        sendObject(43, {}, 0, "list", listAtoms);
        expectPrint("direct-root", "12.5 value");
        sendObject(44, {}, 0, "custom", listAtoms);
        expectPrint("direct-root", "custom 12.5 value");
        sendObject(45, {}, 0, "list", noAtoms);
        expectPrint("direct-root", "bang");
        sendObject(46, { 2 }, 0, "bang", noAtoms);
        expectPrint("direct-nested", "bang");
        sendObject(47, { 2 }, 0, "float", oneFloat);
        expectPrint("direct-nested", "12.5");
        sendObject(48, { 2 }, 0, "symbol", oneSymbol);
        expectPrint("direct-nested", "symbol value");
        sendObject(49, { 2 }, 0, "list", listAtoms);
        expectPrint("direct-nested", "12.5 value");
        sendObject(50, { 2 }, 0, "custom", listAtoms);
        expectPrint("direct-nested", "custom 12.5 value");
        sendObject(51, { 2 }, 0, "list", noAtoms);
        expectPrint("direct-nested", "bang");
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
        sendRequest(makeSendObjectRequest(62, generation, {}, 1, "bang", noAtoms), 62, false, "ObjectTypeMismatch");

        // Boundary tests below dispatch to a dedicated 32-deep fixture's
        // "deep-target" print object rather than the root's "direct-root"
        // print object, so their output never pollutes the exact
        // direct-root/direct-nested print-line assertion made earlier for
        // requests 40-51.
        deepCanvas = editor->getTabComponent().openPatch(makeDeepNestedPatch(32));
        check(deepCanvas != nullptr, "the 32-deep nested fixture canvas must open");
        if (deepCanvas) {
            deepCanvas->performSynchronise();
            auto* deepRoot = deepCanvas->patch.getRawPointer();
            editor->pd->lockAudioThread();
            pd_bind(&deepRoot->gl_obj.ob_pd, editor->pd->generateSymbol("deep-root"));
            editor->pd->unlockAudioThread();

            sendRequest(makeSetGenerationRequest(200, "generation-deep", "deep-root"), 200, true, {});

            // Canvas_path depth exactly 32 succeeds against the real 32-deep
            // fixture; 33 is rejected purely by the static request-shape
            // bound, so no fixture that deep is required for that case.
            Array<int> path32;
            for (int i = 0; i < 32; ++i)
                path32.add(0);
            sendRequest(makeSendObjectRequest(201, "generation-deep", path32, 0, "bang", noAtoms), 201, true, {});
            expectedReplies.back().successStatus = "invoked";

            // Selector length exactly 1024 UTF-8 bytes succeeds; 1025 is rejected.
            sendRequest(makeSendObjectRequest(204, "generation-deep", path32, 0, String::repeatedString("s", 1024), noAtoms), 204, true, {});
            expectedReplies.back().successStatus = "invoked";
            sendRequest(makeSendObjectRequest(205, "generation-deep", path32, 0, String::repeatedString("s", 1025), noAtoms), 205, false, "InvalidSelector");

            // Atom count exactly 256 succeeds via "list"; 257 is rejected.
            Array<var> atoms256;
            for (int i = 0; i < 256; ++i)
                atoms256.add(0.0);
            sendRequest(makeSendObjectRequest(206, "generation-deep", path32, 0, "list", atoms256), 206, true, {});
            expectedReplies.back().successStatus = "invoked";
            Array<var> atoms257 = atoms256;
            atoms257.add(0.0);
            sendRequest(makeSendObjectRequest(207, "generation-deep", path32, 0, "list", atoms257), 207, false, "InvalidRequest");

            // String atom exactly 2048 UTF-8 bytes succeeds; 2049 is rejected.
            Array<var> stringAtom2048 { String::repeatedString("a", 2048) };
            sendRequest(makeSendObjectRequest(208, "generation-deep", path32, 0, "custom", stringAtom2048), 208, true, {});
            expectedReplies.back().successStatus = "invoked";
            Array<var> stringAtom2049 { String::repeatedString("a", 2049) };
            sendRequest(makeSendObjectRequest(209, "generation-deep", path32, 0, "custom", stringAtom2049), 209, false, "InvalidAtoms");

            Array<int> path33 = path32;
            path33.add(0);
            sendRequest(makeSendObjectRequest(202, "generation-deep", path33, 0, "bang", noAtoms), 202, false, "InvalidRequest");

            sendRequest(makeSetGenerationRequest(203, "generation-1", rootReceiver), 203, true, {});
            expectedReplies.back().generationProbe = "generation-1";
            expectedReplies.back().generationActive = true;
        }

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

    void checkExpectedPrintLines(String const& description)
    {
        Array<String> actualDirectPrintLines;
        for (auto const& line : printLines) {
            if (line.startsWith("direct-root: ") || line.startsWith("direct-nested: "))
                actualDirectPrintLines.add(line);
        }
        check(actualDirectPrintLines.size() == expectedPrintLines.size()
                && std::ranges::is_permutation(actualDirectPrintLines, expectedPrintLines),
            description);
    }

    void timerCallback() override
    {
        stopTimer();
        if (consoleStep >= 0) {
            advanceConsoleStep();
            return;
        }
        if (lifetimeStage == 0) {
            checkExpectedPrintLines("print hook must receive exact root and nested lines for every message form");
            check(rootCanvas && rootCanvas->patch.getCanvasContent() == originalCanvasContent,
                "object dispatch must not change serialized patch content while the patch exists");
            deleteRootAndProbe();
        } else if (lifetimeStage == 1) {
            probeDeletedRoot();
        } else if (lifetimeStage == 2) {
            rebuildRootAndProbeStaleGeneration();
        } else if (lifetimeStage == 3) {
            reregisterRootAndProbe();
        } else if (lifetimeStage == 4) {
            advanceLifetimeCycle();
        } else {
            finishProtocol();
        }
    }

    static String makeFixturePatch()
    {
        return "#N canvas 100 100 300 200 12;\n#X obj 20 20 print direct-root;\n#X text 20 50 comment;\n#N canvas 0 0 300 200 nested 0;\n#X obj 20 20 print direct-nested;\n#X restore 100 100 pd nested;\n";
    }

    // Builds a patch with `depth` levels of nesting, each the ordinal-0 child
    // of its parent, so canvas_path = 32 zeros reaches a real target for the
    // canvas-depth boundary test.
    static String makeDeepNestedPatch(int const depth)
    {
        String patch = "#N canvas 100 100 300 200 12;\n";
        for (int i = 1; i <= depth; ++i)
            patch += "#N canvas 0 0 300 200 deep" + String(i) + " 0;\n";
        patch += "#X obj 20 20 print deep-target;\n";
        for (int i = depth; i >= 1; --i)
            patch += "#X restore 100 100 pd deep" + String(i) + ";\n";
        return patch;
    }

    void deleteRootAndProbe()
    {
        editor->pd->lockAudioThread();
        if (auto* root = rootCanvas->patch.getRawPointer())
            pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(rootReceiver));
        editor->pd->unlockAudioThread();

        auto& tabbar = editor->getTabComponent();
        tabbar.closeTab(rootCanvas);
        rootCanvas = nullptr;
        lifetimeStage = 1;
        startTimer(100);
    }

    void probeDeletedRoot()
    {
        lifetimeStage = 2;
        sendRequest(makeSendObjectRequest(70, lifetimeGeneration, {}, 0, "bang", {}), 70, false, "CanvasNotFound");
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
        sendRequest(makeSendObjectRequest(71, lifetimeGeneration, {}, 0, "bang", {}), 71, false, "CanvasNotFound");
        startTimer(5000);
    }

    void reregisterRootAndProbe()
    {
        lifetimeStage = 4;
        String const freshGeneration = "generation-cycle-" + String(lifetimeCycle + 2);
        sendRequest(makeSetGenerationRequest(72, freshGeneration, rootReceiver), 72, true, {});
        sendRequest(makeSendObjectRequest(73, lifetimeGeneration, {}, 0, "bang", {}), 73, false, "StaleGeneration");
        printBuffer.clear();
        printLines.clear();
        expectedPrintLines.clear();
        sendRequest(makeSendObjectRequest(74, freshGeneration, {}, 0, "bang", {}), 74, true, {});
        expectedReplies.back().successStatus = "invoked";
        expectedPrintLines.add("direct-root: bang");
        lifetimeGeneration = freshGeneration;
        startTimer(5000);
    }

    // Repeats delete->probe->rebuild->probe->reregister for lifetimeCycleCount
    // cycles total, proving no cumulative leak or stale access across
    // repeated root destruction/recreation rather than a single pass.
    void advanceLifetimeCycle()
    {
        ++lifetimeCycle;
        if (lifetimeCycle < lifetimeCycleCount) {
            deleteRootAndProbe();
        } else {
            finishProtocol();
        }
    }

    void finishProtocol()
    {
        if (finished)
            return;
        finished = true;
        stopTimer();

        check(responses.size() == static_cast<int>(expectedReplies.size()), "every request must produce exactly one response");
        checkExpectedPrintLines("re-registration must restore the exact rebuilt root print line");
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
        // replyReceiver stays bound for the console phase that follows.
        if (nonCanvasReceiver)
            pd_free(static_cast<t_pd*>(nonCanvasReceiver));
        pd_unbind(static_cast<t_pd*>(printHook), gensym("#plugdata_print"));
        pd_free(static_cast<t_pd*>(printHook));
        pd_bind(static_cast<t_pd*>(editor->pd->printReceiver), gensym("#plugdata_print"));

        if (rootCanvas) {
            if (auto* root = rootCanvas->patch.getRawPointer()) {
                pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(rootReceiver));
            }
        }
        if (deepCanvas) {
            if (auto* root = deepCanvas->patch.getRawPointer()) {
                pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol("deep-root"));
            }
        }
        editor->pd->unlockAudioThread();

        auto& tabbar = editor->getTabComponent();
        while (auto* canvas = tabbar.getCurrentCanvas())
            tabbar.closeTab(canvas);

        setupConsole();
    }

    void setupConsole()
    {
        auto* sidebar = editor->getSidebarForPanel(Sidebar::ConsolePanel);
        consoleComponent = TestHelpers::findChildOfType<Console::ConsoleComponent>(sidebar);
        check(consoleComponent != nullptr, "the console component must be reachable for GUI clear/restore verification");

        consoleStep = 0;
        consoleBegin();
        sendRequest(makeClearConsoleRequest(100), 100, true, {});
        startTimer(5000);
    }

    void runConcurrentProducerProof()
    {
        // Producer A enqueues a warning and signals only after its enqueue
        // completes (the pending mutex is released).
        producerThreadA = std::thread([this] {
            editor->pd->logWarning("producer-A");
            producerAEnqueued.signal();
        });
        producerAEnqueued.wait();
        producerThreadA.join();

        // Producer B blocks until clear_console's critical section has ended,
        // then enqueues an error.
        producerThreadB = std::thread([this] {
            allowProducerB.wait();
            editor->pd->logError("producer-B");
            producerBEnqueued.signal();
        });

        consoleBegin();
        sendRequest(makeClearConsoleRequest(121), 121, true, {});
        consoleStep = 16;
        startTimer(5000);
    }

    void advanceConsoleStep()
    {
        switch (consoleStep) {
        case 0: {
            checkConsoleReply(0, 100, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(), "baseline clear must empty both stores");

            consoleInject("sev-message", 0);
            consoleInject("sev-warning", 1);
            consoleInject("sev-error", 2);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(101, false, 200), 101, true, {});
            consoleStep = 1;
            startTimer(5000);
            break;
        }
        case 1: {
            checkConsoleReply(0, 101, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 3, "severity snapshot must contain exactly three entries");
            if (entries && entries->size() == 3) {
                checkConsoleEntry(entries->getReference(0), "sev-message", "message", 1, "visible", "message entry");
                checkConsoleEntry(entries->getReference(1), "sev-warning", "warning", 1, "visible", "warning entry");
                checkConsoleEntry(entries->getReference(2), "sev-error", "error", 1, "visible", "error entry");
            }
            check(!consoleTruncated(0), "severity snapshot must not be truncated");
            checkConsoleIdsIncreasing(entries, "severity snapshot");

            consoleInject("dup", 1);
            consoleInject("dup", 1);
            consoleInject("dup", 2);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(102, false, 200), 102, true, {});
            consoleStep = 2;
            startTimer(5000);
            break;
        }
        case 2: {
            checkConsoleReply(0, 102, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 5, "repeat snapshot must contain exactly five entries");
            if (entries && entries->size() == 5) {
                checkConsoleEntry(entries->getReference(3), "dup", "warning", 2, "visible", "matching text+severity must collapse");
                checkConsoleEntry(entries->getReference(4), "dup", "error", 1, "visible", "differing severity must not collapse");
            }
            checkConsoleIdsIncreasing(entries, "repeat snapshot");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(110), 110, true, {});
            consoleStep = 3;
            startTimer(5000);
            break;
        }
        case 3: {
            checkConsoleReply(0, 110, true, "");

            for (int i = 0; i < 810; ++i)
                consoleInject("cap-" + String(i), 0);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(103, false, 200), 103, true, {});
            consoleStep = 4;
            startTimer(5000);
            break;
        }
        case 4: {
            checkConsoleReply(0, 103, true, "");
            check(editor->pd->getConsoleMessages().size() == 800, "visible retention cap must stay at 800");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 200, "bounded snapshot must return at most 200 entries");
            if (entries && entries->size() == 200) {
                checkConsoleEntry(entries->getReference(0), "cap-610", "message", 1, "visible", "newest suffix oldest entry");
                checkConsoleEntry(entries->getReference(199), "cap-809", "message", 1, "visible", "newest suffix newest entry");
            }
            checkConsoleIdsIncreasing(entries, "bounded 200-entry snapshot");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(111), 111, true, {});
            consoleStep = 5;
            startTimer(5000);
            break;
        }
        case 5: {
            checkConsoleReply(0, 111, true, "");

            for (int i = 1; i <= 5; ++i)
                consoleInject("order-" + String(i), 0);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(104, false, 1), 104, true, {});
            sendRequest(makeGetConsoleRequest(105, false, 3), 105, true, {});
            sendRequest(makeGetConsoleRequest(106, false, 200), 106, true, {});
            sendRequest(makeGetConsoleRequest(107, false, 201), 107, false, "InvalidRequest");
            sendRequest(makeGetConsoleRequest(108, false, 0), 108, false, "InvalidRequest");
            consoleStep = 6;
            startTimer(5000);
            break;
        }
        case 6: {
            checkConsoleReply(0, 104, true, "");
            checkConsoleReply(1, 105, true, "");
            checkConsoleReply(2, 106, true, "");
            checkConsoleReply(3, 107, false, "InvalidRequest");
            checkConsoleReply(4, 108, false, "InvalidRequest");

            auto const* one = consoleEntries(0);
            check(one != nullptr && one->size() == 1, "max_entries=1 must return exactly one entry");
            if (one && one->size() == 1)
                checkConsoleEntry(one->getReference(0), "order-5", "message", 1, "visible", "max_entries=1 selects the newest entry");

            auto const* three = consoleEntries(1);
            check(three != nullptr && three->size() == 3, "max_entries=3 must return exactly three entries");
            if (three && three->size() == 3) {
                checkConsoleEntry(three->getReference(0), "order-3", "message", 1, "visible", "newest suffix oldest");
                checkConsoleEntry(three->getReference(1), "order-4", "message", 1, "visible", "newest suffix middle");
                checkConsoleEntry(three->getReference(2), "order-5", "message", 1, "visible", "newest suffix newest");
            }

            auto const* all = consoleEntries(2);
            check(all != nullptr && all->size() == 5, "max_entries=200 must return all five entries");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(112), 112, true, {});
            consoleStep = 7;
            startTimer(5000);
            break;
        }
        case 7: {
            checkConsoleReply(0, 112, true, "");

            consoleInject("hist-a", 0);
            consoleInject("hist-b", 1);
            consoleInject("hist-c", 2);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(109, true, 200), 109, true, {});
            consoleStep = 8;
            startTimer(5000);
            break;
        }
        case 8: {
            checkConsoleReply(0, 109, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 3, "history snapshot must contain exactly three visible entries");
            if (entries && entries->size() == 3) {
                checkConsoleEntry(entries->getReference(0), "hist-a", "message", 1, "visible", "visible entry a");
                checkConsoleEntry(entries->getReference(1), "hist-b", "warning", 1, "visible", "visible entry b");
                checkConsoleEntry(entries->getReference(2), "hist-c", "error", 1, "visible", "visible entry c");
            }
            checkConsoleIdsIncreasing(entries, "pre-GUI-clear visible snapshot");
            guiClearIds.clear();
            if (entries)
                for (auto const& entry : *entries) {
                    int64 id = 0;
                    if (consoleEntryId(entry, id))
                        guiClearIds.add(id);
                }
            check(guiClearIds.size() == 3, "GUI clear id capture must observe three ids");

            // Reversible GUI clear moves visible into history.
            if (consoleComponent)
                consoleComponent->clear();
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().size() == 3,
                "GUI clear must move visible entries into history");

            consoleBegin();
            sendRequest(makeGetConsoleRequest(113, false, 200), 113, true, {});
            sendRequest(makeGetConsoleRequest(114, true, 200), 114, true, {});
            consoleStep = 9;
            startTimer(5000);
            break;
        }
        case 9: {
            checkConsoleReply(0, 113, true, "");
            checkConsoleReply(1, 114, true, "");

            auto const* visible = consoleEntries(0);
            check(visible != nullptr && visible->isEmpty(), "include_history=false must exclude history after GUI clear");

            auto const* withHistory = consoleEntries(1);
            check(withHistory != nullptr && withHistory->size() == 3, "include_history=true must surface history entries");
            if (withHistory && withHistory->size() == 3) {
                checkConsoleEntry(withHistory->getReference(0), "hist-a", "message", 1, "history", "history entry a");
                checkConsoleEntry(withHistory->getReference(1), "hist-b", "warning", 1, "history", "history entry b");
                checkConsoleEntry(withHistory->getReference(2), "hist-c", "error", 1, "history", "history entry c");
                for (int i = 0; i < 3; ++i)
                    checkConsoleEntryId(withHistory->getReference(i), guiClearIds[i], "GUI-cleared history entry " + String(i));
            }
            checkConsoleIdsIncreasing(withHistory, "GUI-cleared history snapshot");

            // Reversible GUI restore moves history back into visible.
            if (consoleComponent)
                consoleComponent->restore();
            check(editor->pd->getConsoleMessages().size() == 3 && editor->pd->getConsoleHistory().empty(),
                "GUI restore must move history back into visible");

            consoleBegin();
            sendRequest(makeGetConsoleRequest(115, true, 200), 115, true, {});
            consoleStep = 10;
            startTimer(5000);
            break;
        }
        case 10: {
            checkConsoleReply(0, 115, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 3, "restored entries must be visible again");
            if (entries && entries->size() == 3) {
                checkConsoleEntry(entries->getReference(2), "hist-c", "error", 1, "visible", "restored entry c");
                for (int i = 0; i < 3; ++i)
                    checkConsoleEntryId(entries->getReference(i), guiClearIds[i], "GUI-restored entry " + String(i));
            }
            checkConsoleIdsIncreasing(entries, "GUI-restored visible snapshot");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(116), 116, true, {});
            consoleStep = 11;
            startTimer(5000);
            break;
        }
        case 11: {
            checkConsoleReply(0, 116, true, "");

            consoleInject("small-keep", 0);
            consoleInject(String::repeatedString("z", 70 * 1024), 0);
            consoleInject("small-after", 0);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(117, false, 200), 117, true, {});
            consoleStep = 12;
            startTimer(5000);
            break;
        }
        case 12: {
            checkConsoleReply(0, 117, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 1, "oversized entry must be omitted rather than split");
            if (entries && entries->size() == 1)
                checkConsoleEntry(entries->getReference(0), "small-keep", "message", 1, "visible", "pre-overflow entry survives");
            check(consoleTruncated(0), "oversized-entry snapshot must be truncated");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(118), 118, true, {});
            consoleStep = 13;
            startTimer(5000);
            break;
        }
        case 13: {
            checkConsoleReply(0, 118, true, "");

            consoleInject("wipe-a", 0);
            consoleInject("wipe-b", 1);
            consoleInject("wipe-c", 2);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(119, true, 200), 119, true, {});
            consoleStep = 14;
            startTimer(5000);
            break;
        }
        case 14: {
            checkConsoleReply(0, 119, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 3, "wipe pre-snapshot must contain three entries");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(120), 120, true, {});
            consoleStep = 15;
            startTimer(5000);
            break;
        }
        case 15: {
            checkConsoleReply(0, 120, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(),
                "hard clear must remove visible and history");

            runConcurrentProducerProof();
            break;
        }
        case 16: {
            checkConsoleReply(0, 121, true, "");
            // clear_console's critical section has ended; release producer B.
            allowProducerB.signal();
            producerBEnqueued.wait();
            producerThreadB.join();

            consoleBegin();
            sendRequest(makeGetConsoleRequest(122, false, 200), 122, true, {});
            consoleStep = 17;
            startTimer(5000);
            break;
        }
        case 17: {
            checkConsoleReply(0, 122, true, "");
            auto const* entries = consoleEntries(0);
            bool sawA = false;
            bool sawB = false;
            if (entries) {
                for (auto const& entry : *entries) {
                    auto* obj = entry.getDynamicObject();
                    if (!obj)
                        continue;
                    auto const text = obj->getProperty("text").toString();
                    if (text == "producer-A")
                        sawA = true;
                    if (text == "producer-B")
                        sawB = true;
                }
            }
            check(!sawA, "producer A enqueued before clear must be absent");
            check(sawB, "producer B enqueued after clear must be present");

            runPrintUnderLockConcurrencyProof();
            break;
        }
        case 18: {
            checkConsoleReply(0, 200, true, "");
            for (int i = 0; i < 5; ++i)
                checkConsoleReply(i + 1, 123 + i, true, "");

            stopConcurrentProducers.store(true);
            for (auto& producer : concurrentProducers)
                producer.join();
            concurrentProducers.clear();

            if (concurrencyCanvas) {
                editor->pd->lockAudioThread();
                if (auto* root = concurrencyCanvas->patch.getRawPointer())
                    pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol("concurrency-root"));
                editor->pd->unlockAudioThread();
                editor->getTabComponent().closeTab(concurrencyCanvas);
                concurrencyCanvas = nullptr;
            }

            consoleBegin();
            sendRequest(makeClearConsoleRequest(130), 130, true, {});
            consoleStep = 19;
            startTimer(5000);
            break;
        }
        case 19: {
            checkConsoleReply(0, 130, true, "");

            consoleInject("id-dup", 1);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(131, false, 200), 131, true, {});
            consoleStep = 20;
            startTimer(5000);
            break;
        }
        case 20: {
            checkConsoleReply(0, 131, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 1, "first repeat occurrence must be the only entry");
            if (entries && entries->size() == 1) {
                int64 id = 0;
                check(consoleEntryId(entries->getReference(0), id), "first repeat occurrence must carry an id");
                firstRepeatOccurrenceId = id;
            }

            consoleInject("id-dup", 1);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(132, false, 200), 132, true, {});
            consoleStep = 21;
            startTimer(5000);
            break;
        }
        case 21: {
            checkConsoleReply(0, 132, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 1, "collapsed repeat must remain a single entry");
            if (entries && entries->size() == 1) {
                checkConsoleEntry(entries->getReference(0), "id-dup", "warning", 2, "visible", "repeat collapse with id adoption");
                int64 id = 0;
                check(consoleEntryId(entries->getReference(0), id), "collapsed repeat must carry an id");
                check(id == firstRepeatOccurrenceId + 1, "collapsed repeat id must equal the second occurrence's id");
                check(id > firstRepeatOccurrenceId, "collapsed repeat id must exceed the first occurrence's id");
                lastIdBeforeHardClear = id;
            }

            consoleBegin();
            sendRequest(makeClearConsoleRequest(133), 133, true, {});
            consoleStep = 22;
            startTimer(5000);
            break;
        }
        case 22: {
            checkConsoleReply(0, 133, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(),
                "hard clear before id continuation must empty both stores");

            consoleInject("post-clear-a", 0);
            consoleInject("post-clear-b", 1);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(134, true, 200), 134, true, {});
            consoleStep = 23;
            startTimer(5000);
            break;
        }
        case 23: {
            checkConsoleReply(0, 134, true, "");
            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 2, "post-clear snapshot must contain the two new entries");
            if (entries && entries->size() == 2) {
                int64 firstId = 0;
                int64 secondId = 0;
                consoleEntryId(entries->getReference(0), firstId);
                consoleEntryId(entries->getReference(1), secondId);
                check(firstId > lastIdBeforeHardClear, "post-clear ids must continue past pre-clear ids");
                check(secondId > firstId, "post-clear ids must remain strictly increasing");
            }
            checkConsoleIdsIncreasing(entries, "post-clear snapshot");

            // Cursor section: start from a hard-cleared store with a known id run.
            consoleBegin();
            sendRequest(makeClearConsoleRequest(141), 141, true, {});
            consoleStep = 24;
            startTimer(5000);
            break;
        }
        case 24: {
            checkConsoleReply(0, 141, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(),
                "cursor-section clear must empty both stores");

            for (int i = 1; i <= 5; ++i)
                consoleInject("cur-" + String(i), 0);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(142, false, 200), 142, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 142, true, "");
            auto const* baseline = consoleEntries(0);
            check(baseline != nullptr && baseline->size() == 5, "cursor baseline must return five entries");
            cursorBaseIds.clear();
            if (baseline)
                for (auto const& entry : *baseline) {
                    int64 id = 0;
                    if (consoleEntryId(entry, id))
                        cursorBaseIds.add(id);
                }
            bool const idsOk = cursorBaseIds.size() == 5;
            check(idsOk, "cursor baseline must capture five ids");

            int64 newest = idsOk ? cursorBaseIds[4] : 0;
            int64 interior = idsOk ? cursorBaseIds[1] : 0;
            int64 baselineCursor = 0;
            bool const hasCursor = consoleCursorAt(0, baselineCursor);
            check(hasCursor, "baseline snapshot must carry a cursor");
            if (hasCursor && idsOk)
                check(baselineCursor == newest, "cursor must equal the newest stored id");

            consoleBegin();
            sendRequest(makeGetConsoleRequest(143, false, 200, var(static_cast<int64>(0))), 143, true, {});
            sendRequest(makeGetConsoleRequest(144, false, 200, var(newest)), 144, true, {});
            sendRequest(makeGetConsoleRequest(145, false, 200, var(interior)), 145, true, {});
            sendRequest(makeGetConsoleRequest(146, false, 200, var(newest + 100)), 146, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 143, true, "");
            checkConsoleReply(1, 144, true, "");
            checkConsoleReply(2, 145, true, "");
            checkConsoleReply(3, 146, true, "");

            auto const* all = consoleEntries(0);
            check(all != nullptr && all->size() == 5, "since_id 0 must return all eligible entries");
            if (all && all->size() == 5) {
                checkConsoleEntry(all->getReference(0), "cur-1", "message", 1, "visible", "since_id 0 oldest entry");
                if (idsOk)
                    checkConsoleEntryId(all->getReference(0), cursorBaseIds[0], "since_id 0 first id");
            }

            auto const* emptyFromNewest = consoleEntries(1);
            check(emptyFromNewest != nullptr && emptyFromNewest->isEmpty(), "since_id equal to the newest id must return no entries");

            auto const* suffix = consoleEntries(2);
            check(suffix != nullptr && suffix->size() == 3, "since_id one below an interior id must return the remaining suffix");
            if (suffix && suffix->size() == 3) {
                checkConsoleEntry(suffix->getReference(0), "cur-3", "message", 1, "visible", "filtered suffix oldest");
                if (idsOk) {
                    checkConsoleEntryId(suffix->getReference(0), cursorBaseIds[2], "filtered suffix first id");
                    checkConsoleEntryId(suffix->getReference(2), cursorBaseIds[4], "filtered suffix newest id");
                }
            }
            checkConsoleIdsIncreasing(suffix, "filtered since_id suffix");

            auto const* emptyFromBeyond = consoleEntries(3);
            check(emptyFromBeyond != nullptr && emptyFromBeyond->isEmpty(), "since_id beyond every stored id must return no entries");

            int64 cursorAfterNewest = 0;
            int64 cursorAfterBeyond = 0;
            check(consoleCursorAt(1, cursorAfterNewest), "empty since_id reply must carry a cursor");
            check(consoleCursorAt(3, cursorAfterBeyond), "beyond-newest since_id reply must carry a cursor");
            if (idsOk)
                check(cursorAfterNewest == newest && cursorAfterBeyond == newest, "cursor must equal the newest stored id for empty filters");

            consoleBegin();
            sendJson(makeRawGetConsoleRequest(150, "false", "200", "\"abc\""), 150, false, "InvalidRequest");
            sendJson(makeRawGetConsoleRequest(151, "false", "200", "1.5"), 151, false, "InvalidRequest");
            sendJson(makeRawGetConsoleRequest(152, "false", "200", "-1"), 152, false, "InvalidRequest");
            sendJson(makeRawGetConsoleRequest(153, "false", "200", "true"), 153, false, "InvalidRequest");
            sendJson(makeRawGetConsoleRequest(154, "false", "200", "null"), 154, false, "InvalidRequest");
            sendJson(makeRawGetConsoleRequest(155, "false", "200", "{\"nested\":1}"), 155, false, "InvalidRequest");
            sendJson(makeRawGetConsoleRequest(156, "false", "", "0"), 156, false, "InvalidRequest");
            sendJson("{\"version\":1,\"request_id\":157,\"operation\":\"get_console\",\"include_history\":false,\"max_entries\":200,\"bogus\":1}", 157, false, "InvalidRequest");
            flushDebugQueue();

            for (int i = 0; i < 7; ++i)
                checkConsoleReply(i, 150 + i, false, "InvalidRequest");
            checkConsoleReply(7, 157, false, "InvalidRequest");
            for (int i = 0; i < 6; ++i)
                checkConsoleErrorMessage(i, "since_id must be a non-negative integer");
            checkConsoleErrorMessage(6, "get_console contains unknown or missing fields");
            checkConsoleErrorMessage(7, "get_console contains unknown or missing fields");

            static String const malformedVariants[] = { "string", "fractional", "negative", "true", "null", "nested object" };
            for (int i = 0; i < 6; ++i)
                std::cout << "[malformed since_id] variant=" << malformedVariants[i]
                          << " request_id=" << (150 + i)
                          << " -> InvalidRequest message=\"" << consoleErrorMessage(i) << "\"" << std::endl;
            std::cout << "[malformed since_id] variant=missing max_entries with since_id request_id=156"
                      << " -> InvalidRequest message=\"" << consoleErrorMessage(6) << "\"" << std::endl;
            std::cout << "[malformed since_id] variant=unknown extra field request_id=157"
                      << " -> InvalidRequest message=\"" << consoleErrorMessage(7) << "\"" << std::endl;

            // Malformed since_id must reject before the drain, so a pending row
            // enqueued immediately before the request must still be pending.
            static String const malformedValues[] = { "\"abc\"", "1.5", "-1", "true", "null", "{\"nested\":1}" };
            static int const malformedIds[] = { 160, 161, 162, 163, 164, 165 };
            for (int i = 0; i < 6; ++i) {
                consoleInject("cursor-guard-" + String(i), 0);
                int const before = editor->pd->getConsoleMessages().size();
                consoleBegin();
                sendJson(makeRawGetConsoleRequest(malformedIds[i], "false", "200", malformedValues[i]), malformedIds[i], false, "InvalidRequest");
                flushDebugQueue();
                check(editor->pd->getConsoleMessages().size() == before,
                    "malformed since_id " + String(malformedValues[i]) + " must not drain pending console rows");
                checkConsoleReply(0, malformedIds[i], false, "InvalidRequest");
                checkConsoleErrorMessage(0, "since_id must be a non-negative integer");
            }

            consoleInject("cursor-guard-missing-max", 0);
            int const beforeMissingMax = editor->pd->getConsoleMessages().size();
            consoleBegin();
            sendJson(makeRawGetConsoleRequest(166, "false", "", "0"), 166, false, "InvalidRequest");
            flushDebugQueue();
            check(editor->pd->getConsoleMessages().size() == beforeMissingMax,
                "since_id with missing max_entries must not drain pending console rows");
            checkConsoleReply(0, 166, false, "InvalidRequest");
            checkConsoleErrorMessage(0, "get_console contains unknown or missing fields");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(167), 167, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 167, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(),
                "cursor-section cleanup clear must empty both stores");

            int64 const retained = idsOk ? cursorBaseIds[4] : 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(170, true, 200), 170, true, {});
            sendRequest(makeGetConsoleRequest(171, true, 200, var(retained)), 171, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 170, true, "");
            checkConsoleReply(1, 171, true, "");

            auto const* cleared = consoleEntries(0);
            check(cleared != nullptr && cleared->isEmpty(), "cleared store must return no entries");
            int64 clearedCursor = 0;
            check(consoleCursorAt(0, clearedCursor), "cleared snapshot must carry a cursor");
            if (idsOk)
                check(clearedCursor == retained, "cursor must retain the high-water id after hard clear");

            auto const* emptiedBySince = consoleEntries(1);
            check(emptiedBySince != nullptr && emptiedBySince->isEmpty(), "since_id at the retained cursor must return no entries");
            int64 sinceCursor = 0;
            check(consoleCursorAt(1, sinceCursor), "since_id-at-cursor reply must carry a cursor");
            if (idsOk)
                check(sinceCursor == retained, "cursor must stay at the retained high-water after hard clear");

            consoleStep = 25;
            startTimer(10);
            break;
        }
        // (a) Incremental-drain proof: consecutive injected texts are pairwise
        // distinct, so repeat collapse cannot merge any of them. Each round is
        // drained with a since_id poll; the concatenation of the returned rows
        // must reproduce every injected text exactly once, in id order, with no
        // repeated ids and no missed rows, and the last cursor must equal the
        // final delivered id.
        case 25: {
            String const drainTexts[] = {
                "drain-1", "drain-2", "drain-3", "drain-4",
                "drain-5", "drain-6", "drain-7", "drain-8",
                "drain-9", "drain-10", "drain-11", "drain-12",
            };
            static constexpr int drainRounds = 3;
            static constexpr int drainPerRound = 4;

            Array<String> drainedTexts;
            Array<int64> drainedIds;
            int64 pollCursor = 0;

            consoleBegin();
            sendRequest(makeGetConsoleRequest(180, true, 200), 180, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 180, true, "");
            auto const* baseline = consoleEntries(0);
            check(baseline != nullptr && baseline->isEmpty(), "(a) incremental-drain baseline must be empty");
            check(consoleCursorAt(0, pollCursor), "(a) baseline poll must carry a cursor");

            bool roundsOk = true;
            for (int round = 0; round < drainRounds && roundsOk; ++round) {
                for (int i = 0; i < drainPerRound; ++i)
                    consoleInject(drainTexts[round * drainPerRound + i], 0);

                int const requestId = 181 + round;
                consoleBegin();
                sendRequest(makeGetConsoleRequest(requestId, false, 200, var(pollCursor)), requestId, true, {});
                flushDebugQueue();
                checkConsoleReply(0, requestId, true, "");

                auto const* entries = consoleEntries(0);
                if (entries == nullptr || entries->size() != drainPerRound) {
                    roundsOk = false;
                    check(false, "(a) each round poll must return exactly the injected rows");
                    break;
                }
                checkConsoleIdsIncreasing(entries, "(a) round " + String(round) + " ids");
                for (int i = 0; i < drainPerRound; ++i) {
                    auto const expected = drainTexts[round * drainPerRound + i];
                    checkConsoleEntry(entries->getReference(i), expected, "message", 1, "visible",
                        "(a) round " + String(round) + " entry " + String(i));
                    int64 id = 0;
                    if (consoleEntryId(entries->getReference(i), id)) {
                        drainedIds.add(id);
                        drainedTexts.add(expected);
                    }
                }

                int64 replyCursor = pollCursor;
                check(consoleCursorAt(0, replyCursor), "(a) round poll must carry a cursor");
                int64 lastRoundId = 0;
                consoleEntryId(entries->getReference(drainPerRound - 1), lastRoundId);
                check(replyCursor == lastRoundId, "(a) round cursor must equal the round's newest id");
                if (replyCursor < pollCursor) {
                    roundsOk = false;
                    check(false, "(a) cursor must not move backwards");
                    break;
                }
                pollCursor = replyCursor;
            }

            consoleBegin();
            sendRequest(makeGetConsoleRequest(185, false, 200, var(pollCursor)), 185, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 185, true, "");
            auto const* exhausted = consoleEntries(0);
            check(exhausted != nullptr && exhausted->isEmpty(), "(a) polling from the final cursor must drain to empty");

            check(drainedTexts.size() == drainRounds * drainPerRound, "(a) every injected text must be delivered exactly once");
            check(drainedIds.size() == drainedTexts.size(), "(a) every delivered text must carry an id");
            bool idsUnique = true;
            for (int i = 1; i < drainedIds.size(); ++i)
                if (drainedIds[i] <= drainedIds[i - 1]) {
                    idsUnique = false;
                    break;
                }
            check(idsUnique, "(a) delivered ids must be strictly increasing with no repeats");
            if (drainedIds.size() > 0)
                check(pollCursor == drainedIds[drainedIds.size() - 1], "(a) ending cursor must equal the final id");

            std::cout << "[cursor-lifecycle] (a) incremental drain passed texts=" << drainedTexts.size() << std::endl;
            consoleStep = 26;
            startTimer(10);
            break;
        }
        // (b) Repeat-update observability: a collapsed repeat advances the row's
        // id, so a poll taken after the repeat must deliver the same row again
        // under its newer id with repeats: 2, and a later poll must be empty.
        case 26: {
            consoleBegin();
            sendRequest(makeClearConsoleRequest(190), 190, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 190, true, "");

            int64 cursorBefore = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(191, true, 200), 191, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 191, true, "");
            check(consoleCursorAt(0, cursorBefore), "(b) cleared poll must carry a cursor");

            consoleInject("dup-observe", 1);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(192, false, 200, var(cursorBefore)), 192, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 192, true, "");
            auto const* first = consoleEntries(0);
            check(first != nullptr && first->size() == 1, "(b) first occurrence must be the only row");
            int64 firstId = 0;
            int64 afterFirst = 0;
            if (first && first->size() == 1) {
                checkConsoleEntry(first->getReference(0), "dup-observe", "warning", 1, "visible", "(b) first occurrence");
                check(consoleEntryId(first->getReference(0), firstId), "(b) first occurrence must carry an id");
            }
            check(consoleCursorAt(0, afterFirst), "(b) first poll must carry a cursor");
            check(afterFirst == firstId, "(b) cursor must equal the first occurrence id");

            consoleInject("dup-observe", 1);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(193, false, 200, var(firstId)), 193, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 193, true, "");
            auto const* second = consoleEntries(0);
            check(second != nullptr && second->size() == 1, "(b) repeat advance must re-deliver the row");
            int64 secondId = 0;
            int64 afterSecond = 0;
            if (second && second->size() == 1) {
                checkConsoleEntry(second->getReference(0), "dup-observe", "warning", 2, "visible", "(b) repeat-updated row");
                check(consoleEntryId(second->getReference(0), secondId), "(b) repeat-updated row must carry an id");
                check(secondId > firstId, "(b) repeat-updated id must advance");
            }
            check(consoleCursorAt(0, afterSecond), "(b) repeat poll must carry a cursor");
            check(afterSecond == secondId, "(b) cursor must equal the repeat-updated id");

            consoleBegin();
            sendRequest(makeGetConsoleRequest(194, false, 200, var(secondId)), 194, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 194, true, "");
            auto const* exhaustedRepeat = consoleEntries(0);
            check(exhaustedRepeat != nullptr && exhaustedRepeat->isEmpty(), "(b) polling past the repeat-updated id must be empty");
            int64 stableRepeat = 0;
            check(consoleCursorAt(0, stableRepeat), "(b) exhausted repeat poll must carry a cursor");
            check(stableRepeat == secondId, "(b) cursor must stay at the repeat-updated id");

            std::cout << "[cursor-lifecycle] (b) repeat-update observability passed" << std::endl;
            consoleStep = 27;
            startTimer(10);
            break;
        }
        // (c) GUI clear/restore: whole rows move between history and visible, so
        // ids survive unchanged and the cursor never moves.
        case 27: {
            consoleBegin();
            sendRequest(makeClearConsoleRequest(195), 195, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 195, true, "");

            int64 startCursor = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(196, true, 200), 196, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 196, true, "");
            check(consoleCursorAt(0, startCursor), "(c) baseline poll must carry a cursor");

            consoleInject("guiclr-a", 0);
            consoleInject("guiclr-b", 1);
            consoleInject("guiclr-c", 2);

            String const guiTexts[] = { "guiclr-a", "guiclr-b", "guiclr-c" };
            String const guiSeverities[] = { "message", "warning", "error" };

            Array<int64> guiIds;
            int64 visibleCursor = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(197, true, 200, var(startCursor)), 197, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 197, true, "");
            auto const* visible = consoleEntries(0);
            check(visible != nullptr && visible->size() == 3, "(c) three visible rows must exist before GUI clear");
            if (visible && visible->size() == 3) {
                for (int i = 0; i < 3; ++i) {
                    checkConsoleEntry(visible->getReference(i), guiTexts[i], guiSeverities[i], 1, "visible",
                        "(c) pre-clear visible entry " + String(i));
                    int64 id = 0;
                    if (consoleEntryId(visible->getReference(i), id))
                        guiIds.add(id);
                }
            }
            check(guiIds.size() == 3, "(c) GUI clear id capture must observe three ids");
            check(consoleCursorAt(0, visibleCursor), "(c) visible poll must carry a cursor");

            if (consoleComponent)
                consoleComponent->clear();
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().size() == 3,
                "(c) GUI clear must move visible rows into history");

            int64 historyCursor = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(198, true, 200, var(startCursor)), 198, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 198, true, "");
            auto const* history = consoleEntries(0);
            check(history != nullptr && history->size() == 3, "(c) since_id below the cleared rows must surface history");
            if (history && history->size() == 3) {
                checkConsoleIdsIncreasing(history, "(c) history snapshot");
                for (int i = 0; i < 3; ++i) {
                    checkConsoleEntry(history->getReference(i), guiTexts[i], guiSeverities[i], 1, "history",
                        "(c) history entry " + String(i));
                    if (guiIds.size() == 3)
                        checkConsoleEntryId(history->getReference(i), guiIds[i], "(c) history entry id " + String(i));
                }
            }
            check(consoleCursorAt(0, historyCursor), "(c) history poll must carry a cursor");
            check(historyCursor == visibleCursor, "(c) cursor must be unchanged across GUI clear");

            if (consoleComponent)
                consoleComponent->restore();
            check(editor->pd->getConsoleMessages().size() == 3 && editor->pd->getConsoleHistory().empty(),
                "(c) GUI restore must move history back into visible");

            int64 restoredCursor = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(199, true, 200, var(startCursor)), 199, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 199, true, "");
            auto const* restored = consoleEntries(0);
            check(restored != nullptr && restored->size() == 3, "(c) restored rows must be visible again");
            if (restored && restored->size() == 3) {
                checkConsoleIdsIncreasing(restored, "(c) restored snapshot");
                for (int i = 0; i < 3; ++i) {
                    checkConsoleEntry(restored->getReference(i), guiTexts[i], guiSeverities[i], 1, "visible",
                        "(c) restored entry " + String(i));
                    if (guiIds.size() == 3)
                        checkConsoleEntryId(restored->getReference(i), guiIds[i], "(c) restored entry id " + String(i));
                }
            }
            check(consoleCursorAt(0, restoredCursor), "(c) restored poll must carry a cursor");
            check(restoredCursor == visibleCursor, "(c) cursor must be unchanged across GUI restore");

            std::cout << "[cursor-lifecycle] (c) GUI clear/restore id preservation passed" << std::endl;
            consoleStep = 28;
            startTimer(10);
            break;
        }
        // (d) Hard clear: ids are never reset, the cursor retains its high-water,
        // and post-clear rows continue above every pre-clear id. A poll from the
        // pre-clear cursor returns exactly the new rows.
        case 28: {
            Array<int64> preClearIds;
            int64 preClearCursor = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(200, true, 200), 200, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 200, true, "");
            auto const* pre = consoleEntries(0);
            check(pre != nullptr && pre->size() == 3, "(d) three rows must exist before hard clear");
            if (pre)
                for (auto const& entry : *pre) {
                    int64 id = 0;
                    if (consoleEntryId(entry, id))
                        preClearIds.add(id);
                }
            check(preClearIds.size() == 3, "(d) hard-clear id capture must observe three ids");
            check(consoleCursorAt(0, preClearCursor), "(d) pre-clear poll must carry a cursor");

            consoleBegin();
            sendRequest(makeClearConsoleRequest(201), 201, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 201, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(),
                "(d) hard clear must empty both stores");

            consoleBegin();
            sendRequest(makeGetConsoleRequest(202, true, 200), 202, true, {});
            sendRequest(makeGetConsoleRequest(203, true, 200, var(preClearCursor)), 203, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 202, true, "");
            checkConsoleReply(1, 203, true, "");
            auto const* clearedAbsent = consoleEntries(0);
            check(clearedAbsent != nullptr && clearedAbsent->isEmpty(), "(d) post-clear entries must be empty");
            int64 retainedCursor = 0;
            check(consoleCursorAt(0, retainedCursor), "(d) post-clear poll must carry a cursor");
            check(retainedCursor == preClearCursor, "(d) cursor must be retained across hard clear");
            auto const* clearedSince = consoleEntries(1);
            check(clearedSince != nullptr && clearedSince->isEmpty(), "(d) since_id at the pre-clear cursor must be empty after clear");

            consoleInject("hc-new-a", 0);
            consoleInject("hc-new-b", 1);
            consoleBegin();
            sendRequest(makeGetConsoleRequest(204, true, 200, var(preClearCursor)), 204, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 204, true, "");
            auto const* fresh = consoleEntries(0);
            check(fresh != nullptr && fresh->size() == 2, "(d) poll from the pre-clear cursor must return exactly the two new rows");
            if (fresh && fresh->size() == 2) {
                checkConsoleEntry(fresh->getReference(0), "hc-new-a", "message", 1, "visible", "(d) new row a");
                checkConsoleEntry(fresh->getReference(1), "hc-new-b", "warning", 1, "visible", "(d) new row b");
                int64 first = 0;
                int64 second = 0;
                consoleEntryId(fresh->getReference(0), first);
                consoleEntryId(fresh->getReference(1), second);
                check(first > preClearCursor, "(d) new ids must exceed the retained cursor");
                if (preClearIds.size() == 3) {
                    check(first > preClearIds[2], "(d) new ids must exceed every pre-clear id");
                    check(second > preClearIds[2], "(d) the second new id must exceed every pre-clear id");
                }
                check(second > first, "(d) new ids must remain strictly increasing");
                int64 afterFresh = 0;
                check(consoleCursorAt(0, afterFresh), "(d) fresh poll must carry a cursor");
                check(afterFresh == second, "(d) cursor must equal the newest new id");
            }
            checkConsoleIdsIncreasing(fresh, "(d) post-clear new rows");

            std::cout << "[cursor-lifecycle] (d) hard clear retention and id continuation passed" << std::endl;
            consoleStep = 29;
            startTimer(10);
            break;
        }
        // (e) Concurrent producers. Worker threads hammer logWarning while the
        // message thread polls get_console with since_id. Only externally
        // decidable invariants are asserted: every single poll returns strictly
        // increasing ids, no observed id exceeds the final cursor, the final
        // poll after producers join drains to empty, and the cursor stabilizes.
        // Gap attribution is not decidable (most hammered ids collapse by
        // design), so no gap-count assertion is made.
        case 29: {
            int64 pollCursor = 0;
            consoleBegin();
            sendRequest(makeGetConsoleRequest(210, true, 200), 210, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 210, true, "");
            check(consoleCursorAt(0, pollCursor), "(e) pre-concurrency poll must carry a cursor");

            stopConcurrentProducers.store(false);
            concurrentProducers.clear();
            for (int i = 0; i < 3; ++i) {
                concurrentProducers.emplace_back([this, i] {
                    String const text = "concurrent-noise-" + String(i);
                    while (!stopConcurrentProducers.load())
                        editor->pd->logWarning(text);
                });
            }

            static constexpr int concurrencyPollCount = 120;
            bool idsMonotonic = true;
            int64 maxObservedId = pollCursor;
            for (int i = 0; i < concurrencyPollCount && idsMonotonic; ++i) {
                int const requestId = 211 + i;
                consoleBegin();
                sendRequest(makeGetConsoleRequest(requestId, false, 200, var(pollCursor)), requestId, true, {});
                flushDebugQueue();
                checkConsoleReply(0, requestId, true, "");

                auto const* entries = consoleEntries(0);
                if (entries == nullptr) {
                    idsMonotonic = false;
                    check(false, "(e) concurrent poll must expose an entries array");
                    break;
                }
                int64 previous = 0;
                for (auto const& entry : *entries) {
                    int64 id = 0;
                    if (!consoleEntryId(entry, id) || id <= previous) {
                        idsMonotonic = false;
                        break;
                    }
                    previous = id;
                    maxObservedId = std::max(maxObservedId, id);
                }
                if (!idsMonotonic)
                    break;

                int64 replyCursor = pollCursor;
                check(consoleCursorAt(0, replyCursor), "(e) concurrent poll must carry a cursor");
                maxObservedId = std::max(maxObservedId, replyCursor);
                if (replyCursor < pollCursor) {
                    idsMonotonic = false;
                    break;
                }
                pollCursor = replyCursor;
            }
            check(idsMonotonic, "(e) every concurrent poll must return strictly increasing ids");

            stopConcurrentProducers.store(true);
            for (auto& producer : concurrentProducers)
                producer.join();
            concurrentProducers.clear();

            int64 finalCursor = pollCursor;
            bool drained = false;
            for (int i = 0; i < 50; ++i) {
                int const requestId = 400 + i;
                consoleBegin();
                sendRequest(makeGetConsoleRequest(requestId, true, 200, var(finalCursor)), requestId, true, {});
                flushDebugQueue();
                checkConsoleReply(0, requestId, true, "");
                auto const* entries = consoleEntries(0);
                int64 replyCursor = finalCursor;
                check(consoleCursorAt(0, replyCursor), "(e) drain poll must carry a cursor");
                maxObservedId = std::max(maxObservedId, replyCursor);
                if (replyCursor < finalCursor)
                    break;
                finalCursor = replyCursor;
                if (entries != nullptr && entries->isEmpty()) {
                    drained = true;
                    break;
                }
            }
            check(drained, "(e) a final poll after producers join must drain to empty");

            int64 stableCursor = finalCursor;
            for (int i = 0; i < 2; ++i) {
                int const requestId = 450 + i;
                consoleBegin();
                sendRequest(makeGetConsoleRequest(requestId, true, 200, var(stableCursor)), requestId, true, {});
                flushDebugQueue();
                checkConsoleReply(0, requestId, true, "");
                auto const* entries = consoleEntries(0);
                check(entries != nullptr && entries->isEmpty(), "(e) post-join polls must stay empty");
                int64 replyCursor = stableCursor;
                check(consoleCursorAt(0, replyCursor), "(e) stabilizing poll must carry a cursor");
                check(replyCursor == stableCursor, "(e) cursor must stabilize after producers join");
                stableCursor = replyCursor;
            }

            check(maxObservedId <= finalCursor, "(e) no observed id may exceed the final cursor");

            std::cout << "[cursor-lifecycle] (e) concurrent producers passed polls=" << concurrencyPollCount
                      << " drained=" << (drained ? 1 : 0) << std::endl;
            consoleStep = 30;
            startTimer(10);
            break;
        }
        // (f) 800-cap eviction. The visible store retains only the newest 800
        // rows, so a cursor-0 poll cannot reach evicted rows; the returned window
        // is bounded by max_entries and is not byte-truncated for short rows.
        case 30: {
            consoleBegin();
            sendRequest(makeClearConsoleRequest(500), 500, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 500, true, "");
            check(editor->pd->getConsoleMessages().empty() && editor->pd->getConsoleHistory().empty(),
                "(f) eviction fixture must start empty");

            for (int i = 0; i < 810; ++i)
                consoleInject("evict-" + String(i), 0);

            consoleBegin();
            sendRequest(makeGetConsoleRequest(501, true, 200, var(static_cast<int64>(0))), 501, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 501, true, "");
            check(editor->pd->getConsoleMessages().size() == 800, "(f) visible retention cap must stay at 800");

            auto const* entries = consoleEntries(0);
            check(entries != nullptr && entries->size() == 200, "(f) cursor-0 poll must return the newest 200-row window");
            if (entries && entries->size() == 200) {
                checkConsoleEntry(entries->getReference(0), "evict-610", "message", 1, "visible",
                    "(f) oldest reachable row must be evict-610 (evicted rows are unreachable)");
                checkConsoleEntry(entries->getReference(199), "evict-809", "message", 1, "visible",
                    "(f) newest reachable row must be evict-809");
                checkConsoleIdsIncreasing(entries, "(f) eviction window");
            }
            check(!consoleTruncated(0), "(f) a within-window poll must not be byte-truncated");

            std::cout << "[cursor-lifecycle] (f) 800-cap eviction bounded window passed" << std::endl;
            consoleStep = 31;
            startTimer(10);
            break;
        }
        // (export) Task 1: export_canvas happy path. Opens a dedicated fixture
        // canvas with one object inside the visible viewport, registers a
        // generation, and exports the visible content region to a unique temp
        // file; the PNG must decode with exactly the region's pixel dimensions.
        case 31: {
            exportRootReceiver = "export-root";
            exportCanvas = editor->getTabComponent().openPatch(makeExportFixturePatch());
            check(exportCanvas != nullptr, "the export fixture canvas must open");
            if (!exportCanvas) {
                finishConsole();
                break;
            }
            exportCanvas->performSynchronise();
            auto* exportRoot = exportCanvas->patch.getRawPointer();
            editor->pd->lockAudioThread();
            pd_bind(&exportRoot->gl_obj.ob_pd, editor->pd->generateSymbol(exportRootReceiver));
            editor->pd->unlockAudioThread();

            consoleBegin();
            sendRequest(makeSetGenerationRequest(600, "export-generation", exportRootReceiver), 600, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 600, true, "");
            exportGeneration = "export-generation";

            // Start from the patch origin at 100% zoom so the fixture object is
            // deterministically inside the visible viewport.
            exportCanvas->restoreViewportState();
            check(std::abs(getValue<float>(exportCanvas->zoomScale) - 1.0f) < 0.001f,
                "export fixture must be pinned at 100% zoom");
            exportExpectedLogical = exportExpectedRegion(exportCanvas);
            check(!exportExpectedLogical.isEmpty(), "export fixture object must be inside the visible viewport");

            File const tempDir = File::getSpecialLocation(File::tempDirectory);
            exportTempFile = tempDir.getChildFile("plugdata-export-" + String(Time::currentTimeMillis()) + ".png");
            exportTempFile.deleteFile();

            consoleBegin();
            sendRequest(makeExportCanvasRequest(601, exportGeneration, exportTempFile.getFullPathName()), 601, true, {});
            flushDebugQueue();
            checkExportSaved(0, false);
            checkExportFile(exportTempFile, exportExpectedLogical, "export");

            exportTempFileUpper = tempDir.getChildFile("plugdata-export-" + String(Time::currentTimeMillis()) + ".PNG");
            exportTempFileUpper.deleteFile();
            consoleBegin();
            sendRequest(makeExportCanvasRequest(602, exportGeneration, exportTempFileUpper.getFullPathName()), 602, true, {});
            flushDebugQueue();
            checkExportSaved(0, false);
            checkExportFile(exportTempFileUpper, exportExpectedLogical, "uppercase-extension export");

            std::cout << "[export] (a) happy path logical=" << exportExpectedLogical.getWidth() << "x"
                      << exportExpectedLogical.getHeight() << " renderScale=" << editor->getRenderScale()
                      << " clipped=false" << std::endl;
            consoleStep = 32;
            startTimer(10);
            break;
        }
        // (export) Request-shape rejections: every malformed field set replies
        // exactly once with InvalidRequest and creates no file.
        case 32: {
            File const tempDir = File::getSpecialLocation(File::tempDirectory);
            String const absolutePng = tempDir.getChildFile("plugdata-export-matrix.png").getFullPathName();
            String const absoluteJpg = tempDir.getChildFile("plugdata-export-matrix.jpg").getFullPathName();

            consoleBegin();
            sendRequest(makeExportCanvasRequest(610, exportGeneration, "relative/path.png"), 610, false, "InvalidRequest");
            sendRequest(makeExportCanvasRequest(611, exportGeneration, absoluteJpg), 611, false, "InvalidRequest");
            sendRequest(makeExportCanvasRequest(612, exportGeneration, ""), 612, false, "InvalidRequest");
            sendRequest(makeExportCanvasRequest(613, exportGeneration, var(42)), 613, false, "InvalidRequest");
            sendRequest(makeExportCanvasRequest(614, "", absolutePng), 614, false, "InvalidRequest");
            sendRequest(makeExportCanvasRequest(615, String::repeatedString("g", 129), absolutePng), 615, false, "InvalidRequest");
            sendRequest(makeExportCanvasRequest(616, exportGeneration, absolutePng, true), 616, false, "InvalidRequest");
            flushDebugQueue();

            for (int i = 0; i < 7; ++i)
                checkConsoleReply(i, 610 + i, false, "InvalidRequest");
            checkConsoleErrorMessage(0, "path must be an absolute file path ending in .png");
            checkConsoleErrorMessage(1, "path must be an absolute file path ending in .png");
            checkConsoleErrorMessage(2, "path must be an absolute file path ending in .png");
            checkConsoleErrorMessage(3, "path must be an absolute file path ending in .png");
            checkConsoleErrorMessage(4, "generation and root_receiver must contain 1 through 128 UTF-8 bytes");
            checkConsoleErrorMessage(5, "generation and root_receiver must contain 1 through 128 UTF-8 bytes");
            checkConsoleErrorMessage(6, "export_canvas contains unknown or missing fields");
            check(!File(absolutePng).existsAsFile(), "rejected export paths must not create a file");

            std::cout << "[export] (b) invalid-request matrix passed cases=7" << std::endl;
            consoleStep = 33;
            startTimer(10);
            break;
        }
        // (export) A live but non-displayed registered canvas is rejected.
        case 33: {
            hiddenExportCanvas = editor->getTabComponent().openPatch(makeFixturePatch());
            check(hiddenExportCanvas != nullptr, "the hidden-canvas fixture must open");
            if (hiddenExportCanvas)
                hiddenExportCanvas->performSynchronise();

            File const tempDir = File::getSpecialLocation(File::tempDirectory);
            String const rejectedPath = tempDir.getChildFile("plugdata-export-hidden.png").getFullPathName();
            File(rejectedPath).deleteFile();

            consoleBegin();
            sendRequest(makeExportCanvasRequest(620, exportGeneration, rejectedPath), 620, false, "CanvasNotFound");
            flushDebugQueue();
            checkConsoleReply(0, 620, false, "CanvasNotFound");
            checkConsoleErrorMessage(0, "canvas is not currently displayed");
            check(!File(rejectedPath).existsAsFile(), "hidden-canvas export must not create a file");

            if (hiddenExportCanvas) {
                editor->getTabComponent().closeTab(hiddenExportCanvas);
                hiddenExportCanvas = nullptr;
            }

            std::cout << "[export] (c) hidden canvas rejected CanvasNotFound" << std::endl;
            consoleStep = 34;
            startTimer(10);
            break;
        }
        // (export) Destroying the registered root while its token stays active
        // invalidates the weak reference (CanvasNotFound); an unknown token is
        // still StaleGeneration.
        case 34: {
            editor->pd->lockAudioThread();
            if (auto* root = exportCanvas->patch.getRawPointer())
                pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(exportRootReceiver));
            editor->pd->unlockAudioThread();

            editor->getTabComponent().closeTab(exportCanvas);
            exportCanvas = nullptr;

            File const tempDir = File::getSpecialLocation(File::tempDirectory);
            String const rejectedPath = tempDir.getChildFile("plugdata-export-deleted.png").getFullPathName();
            File(rejectedPath).deleteFile();

            consoleBegin();
            sendRequest(makeExportCanvasRequest(630, exportGeneration, rejectedPath), 630, false, "CanvasNotFound");
            flushDebugQueue();
            checkConsoleReply(0, 630, false, "CanvasNotFound");
            check(!File(rejectedPath).existsAsFile(), "deleted-root export must not create a file");

            consoleBegin();
            sendRequest(makeExportCanvasRequest(631, "export-generation-stale", rejectedPath), 631, false, "StaleGeneration");
            flushDebugQueue();
            checkConsoleReply(0, 631, false, "StaleGeneration");

            auto& tabbar = editor->getTabComponent();
            while (auto* canvas = tabbar.getCurrentCanvas())
                tabbar.closeTab(canvas);

            if (exportTempFile.existsAsFile())
                exportTempFile.deleteFile();
            if (exportTempFileUpper.existsAsFile())
                exportTempFileUpper.deleteFile();

            std::cout << "[export] (d) deleted root and stale token paths passed" << std::endl;
            finishConsole();
            break;
        }
        default:
            finishConsole();
            break;
        }
    }

    // Proves the Task-3 pending-console mutex never needs to nest with the
    // Pd lock under real contention: producer threads hammer logWarning()
    // (pendingLock only) while the message thread concurrently dispatches
    // send_object bangs to a real [print] target (Pd lock, via
    // dispatchResolvedMessage). A genuine nesting bug would hang this step
    // until the process-level test timeout instead of completing.
    void runPrintUnderLockConcurrencyProof()
    {
        concurrencyCanvas = editor->getTabComponent().openPatch(
            "#N canvas 100 100 300 200 12;\n#X obj 20 20 print concurrency-target;\n");
        check(concurrencyCanvas != nullptr, "the concurrency fixture canvas must open");
        if (!concurrencyCanvas) {
            finishConsole();
            return;
        }
        concurrencyCanvas->performSynchronise();
        auto* root = concurrencyCanvas->patch.getRawPointer();
        editor->pd->lockAudioThread();
        pd_bind(&root->gl_obj.ob_pd, editor->pd->generateSymbol("concurrency-root"));
        editor->pd->unlockAudioThread();

        consoleBegin();
        sendRequest(makeSetGenerationRequest(200, "generation-concurrency", "concurrency-root"), 200, true, {});

        stopConcurrentProducers.store(false);
        for (int i = 0; i < 3; ++i) {
            concurrentProducers.emplace_back([this] {
                while (!stopConcurrentProducers.load())
                    editor->pd->logWarning("concurrent-noise");
            });
        }

        for (int i = 0; i < 5; ++i) {
            sendRequest(makeSendObjectRequest(123 + i, "generation-concurrency", {}, 0, "bang", Array<var>()), 123 + i, true, {});
            expectedReplies.back().successStatus = "invoked";
        }

        consoleStep = 18;
        startTimer(10000);
    }

    void finishConsole()
    {
        if (consoleFinished)
            return;
        consoleFinished = true;
        stopTimer();

        editor->pd->lockAudioThread();
        if (replyReceiver)
            pd_free(static_cast<t_pd*>(replyReceiver));
        replyReceiver = nullptr;
        editor->pd->unlockAudioThread();

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
    Canvas* deepCanvas = nullptr;
    void* replyReceiver = nullptr;
    void* nonCanvasReceiver = nullptr;
    void* printHook = nullptr;
    String rootReceiver;
    String lifetimeGeneration;
    String originalCanvasContent;
    String printBuffer;
    Array<String> printLines;
    Array<String> expectedPrintLines;
    int receivedLifecycleMessages = 0;
    int lifetimeStage = 0;
    int lifetimeCycle = 0;
    static constexpr int lifetimeCycleCount = 5;
    bool allPassed = true;
    bool finished = false;

    int consoleStep = -1;
    bool consoleFinished = false;
    Console::ConsoleComponent* consoleComponent = nullptr;
    std::thread producerThreadA;
    std::thread producerThreadB;
    WaitableEvent producerAEnqueued;
    WaitableEvent allowProducerB;
    WaitableEvent producerBEnqueued;
    Canvas* concurrencyCanvas = nullptr;
    std::vector<std::thread> concurrentProducers;
    std::atomic<bool> stopConcurrentProducers { false };
    int64 firstRepeatOccurrenceId = 0;
    int64 lastIdBeforeHardClear = 0;
    Array<int64> guiClearIds;
    Array<int64> cursorBaseIds;

    Canvas* exportCanvas = nullptr;
    Canvas* hiddenExportCanvas = nullptr;
    String exportRootReceiver;
    String exportGeneration;
    File exportTempFile;
    File exportTempFileUpper;
    Rectangle<int> exportExpectedLogical;
};
