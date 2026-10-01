#include "Pd/Setup.h"
#include "Sidebar/Console.h"
#include "Standalone/PlugDataWindow.h"
#include <atomic>
#include <iostream>
#include <thread>

#if ENABLE_TESTING
namespace pd {
void setCanvasEditInterleaveHookForTesting(void (*hook)(void*), void* context);
void setCanvasEditFailureHookForTesting(void (*hook)(void*), void* context);
void setCanvasEditGuiMismatchHookForTesting(void (*hook)(void*), void* context);
}
#endif

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

    static void forceStaleGenerationAfterTabClose(void* context)
    {
        auto* test = static_cast<DirectDebugApiTest*>(context);
        test->editor->pd->lockAudioThread();
        test->editor->pd->debugGeneration = "tab-delete-interleaved-generation";
        test->editor->pd->unlockAudioThread();
        test->tabDeleteInterleaveHookRan = true;
    }

    static void rebindDebugRootAfterTabClose(void* context)
    {
        auto* test = static_cast<DirectDebugApiTest*>(context);
        bool rebound = false;
        test->editor->pd->lockAudioThread();
        auto* replacementRoot = test->tabDeleteRebindPatch ? test->tabDeleteRebindPatch->getRawPointer() : nullptr;
        if (replacementRoot) {
            test->editor->pd->debugRoot = std::make_unique<pd::WeakReference>(replacementRoot, test->editor->pd);
            rebound = true;
        }
        test->editor->pd->unlockAudioThread();
        test->tabDeleteRootRebindHookRan = rebound;
    }

    static void failAfterFirstCanvasAdd(void* context)
    {
        *static_cast<bool*>(context) = true;
    }

    static void breakAcknowledgmentCanvas(void* context)
    {
        auto* canvas = static_cast<Canvas*>(context);
        canvas->objects.remove_at(canvas->objects.size() - 1);
    }

    static void leavePluginModeDuringCanvasAck(void* context)
    {
        auto* modeEditor = static_cast<PluginEditor*>(context);
        if (auto* canvas = modeEditor->getPluginModeCanvas()) {
            canvas->patch.openInPluginMode = false;
            modeEditor->getTabComponent().updateNow();
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
    // transform (before clipping to the editor).
    Rectangle<int> exportMappedContentRegion(Canvas* canvas) const
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
        return Rectangle<int>::leftTopRightBottom(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
    }

    Rectangle<int> exportExpectedRegion(Canvas* canvas) const
    {
        return exportMappedContentRegion(canvas).getIntersection(editor->getLocalBounds());
    }

    Canvas* openBoundExportFixture(String const& patchContent, String const& receiver)
    {
        auto* canvas = editor->getTabComponent().openPatch(patchContent);
        if (!canvas)
            return nullptr;
        canvas->performSynchronise();
        auto* root = canvas->patch.getRawPointer();
        editor->pd->lockAudioThread();
        pd_bind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(receiver));
        editor->pd->unlockAudioThread();
        return canvas;
    }

    void closeExportFixture(Canvas* canvas, String const& receiver)
    {
        if (!canvas)
            return;
        editor->pd->lockAudioThread();
        if (auto* root = canvas->patch.getRawPointer())
            pd_unbind(&root->gl_obj.ob_pd, editor->pd->generateSymbol(receiver));
        editor->pd->unlockAudioThread();
        editor->getTabComponent().closeTab(canvas);
    }

    void registerExportGeneration(int const requestId, String const& generation, String const& receiver)
    {
        consoleBegin();
        sendRequest(makeSetGenerationRequest(requestId, generation, receiver), requestId, true, {});
        flushDebugQueue();
        checkConsoleReply(0, requestId, true, "");
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

        testCanvasEditWireValidation();
        testCanvasSnapshotPages();
        testCanvasPreflightOrderedRefs();
        testCanvasAddKindsAndRefs();
        testCanvasChildMoveDeleteAndRootSurvivor();
        testCanvasSubpatchDeletionAndRebasing();
        testCanvasOrderedConnectAndDisconnect();
        testCanvasPortMoveRouting();
        testCanvasAcknowledgmentAndMapping();

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
        sendObject(63, {}, 3, "bang", noAtoms);
        expectPrint("direct-root", "content-from-box");

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

    void testCanvasEditWireValidation()
    {
        beginTest("Canvas edit wire validation");
        capturingCanvasWire = true;
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const before = rootCanvas->patch.getCanvasContent();
        auto const sendCanvasJson = [this, &before](int const id, String const& fields, String const& code) {
            consoleBegin();
            String const request = "{\"version\":1,\"request_id\":" + String(id) + "," + fields + "}";
            if (id == 300 || id == 303)
                std::cout << "[canvas-wire] id=" << id << " request=" << request
                          << " encoded-request=" << Base64::toBase64(request.toRawUTF8(), request.getNumBytesAsUTF8()) << std::endl;
            sendJson(request, id, code.isEmpty(), code);
            flushDebugQueue();
            check(responses.size() == 1, "canvas request must receive exactly one reply for request " + String(id));
            checkConsoleReply(0, id, code.isEmpty(), code);
            check(rootCanvas->patch.getCanvasContent() == before, "canvas validation must leave the Pd fixture unchanged for request " + String(id));
            auto* response = responses.isEmpty() ? nullptr : responses[0].getDynamicObject();
            if (response)
                std::cout << "[canvas-wire] id=" << id << " raw=" << JSON::toString(responses[0], true) << std::endl;
        };
        String const read = "\"operation\":\"read_canvas\",\"generation\":\"generation-1\",\"canvas_path\":[]";
        String const add = "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"child\",\"type\":\"subpatch\",\"x\":10,\"y\":20,\"name\":\"nested\"}";
        String const nested = "{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"child\"}],\"ref\":\"note\",\"type\":\"comment\",\"x\":0,\"y\":0,\"text\":\"hello\"}";
        sendCanvasJson(300, read, "");
        auto* readEnvelope = responses.isEmpty() ? nullptr : responses[0].getDynamicObject();
        auto* readData = readEnvelope ? readEnvelope->getProperty("data").getDynamicObject() : nullptr;
        String const revision = readData ? readData->getProperty("revision").toString() : String();
        check(revision.length() == 64, "wire validation must use the live root revision for valid edit requests");
        String const edit = "\"operation\":\"edit_canvas\",\"generation\":\"generation-1\",\"expected_revision\":\"" + revision + "\",\"edits\":";
        sendCanvasJson(301, read + ",\"cursor\":\"page-1\",\"expected_revision\":\"revision-1\"", "StaleRevision");
        sendCanvasJson(320, read + ",\"cursor\":\"page-1\"", "InvalidRequest");
        sendCanvasJson(302, edit + "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":20,\"y\":20}]", "");
        sendCanvasJson(303, read + ",\"extra\":true", "InvalidRequest");
        sendCanvasJson(304, read + ",\"canvas_path_override\":[]", "InvalidRequest");
        sendCanvasJson(305, "\"operation\":\"read_canvas\",\"generation\":\"generation-1\",\"canvas_path\":[{\"ref\":\"child\"}]", "InvalidRequest");
        sendCanvasJson(306, "\"operation\":\"read_canvas\",\"generation\":\"generation-1\",\"canvas_path\":[-1]", "InvalidRequest");
        sendCanvasJson(307, edit + "[" + add + "," + add + "]", "InvalidRequest");
        sendCanvasJson(308, edit + "[" + nested + "," + add + "]", "InvalidRequest");
        sendCanvasJson(309, edit + "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"bad\",\"type\":\"object\",\"x\":1.5,\"y\":0,\"text\":\"print\"}]", "InvalidRequest");
        sendCanvasJson(310, edit + "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"bad\",\"type\":\"object\",\"x\":1000001,\"y\":0,\"text\":\"print\"}]", "InvalidRequest");
        sendCanvasJson(311, edit + "[" + add.dropLastCharacters(1) + ",\"unexpected\":1}]", "InvalidRequest");
        sendCanvasJson(312, edit + "[" + add + ",{\"op\":\"remove\",\"canvas_path\":[],\"target\":{\"ref\":\"child\"}}," + nested + "]", "InvalidRequest");
        sendCanvasJson(313, edit + "[" + add + ",{\"op\":\"remove\",\"canvas_path\":[],\"target\":{\"ref\":\"child\"}}," + add + "]", "InvalidRequest");
        sendCanvasJson(314, edit + "[" + add + ",{\"op\":\"connect\",\"canvas_path\":[],\"source\":{\"ref\":\"child\"},\"outlet\":-1,\"target\":0,\"inlet\":0}]", "InvalidRequest");
        StringArray edits;
        for (int i = 0; i < 101; ++i)
            edits.add("{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":20,\"y\":20}");
        StringArray const hundredEdits = [&edits] {
            auto copy = edits;
            copy.remove(copy.size() - 1);
            return copy;
        }();
        sendCanvasJson(318, edit + "[" + hundredEdits.joinIntoString(",") + "]", "");
        sendCanvasJson(315, edit + "[" + edits.joinIntoString(",") + "]", "InvalidRequest");
        Array<var> path;
        for (int i = 0; i < 33; ++i)
            path.add(0);
        path.remove(path.size() - 1);
        sendCanvasJson(319, "\"operation\":\"read_canvas\",\"generation\":\"generation-1\",\"canvas_path\":" + JSON::toString(var(path), true), "CanvasTypeMismatch");
        path.add(0);
        sendCanvasJson(316, "\"operation\":\"read_canvas\",\"generation\":\"generation-1\",\"canvas_path\":" + JSON::toString(var(path), true), "InvalidRequest");
        sendCanvasJson(317, edit + "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"bad\",\"type\":\"comment\",\"x\":0,\"y\":0,\"text\":\"" + String::repeatedString("x", 2049) + "\"}]", "InvalidRequest");
        consoleBegin();
        expectedReplies = previousReplies;
        responses = previousResponses;
        capturingCanvasWire = false;
        std::cout << "[canvas-wire] fixture unchanged after valid and invalid requests" << std::endl;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasSnapshotPages()
    {
        beginTest("Canvas snapshot pages");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String fixture = "#N canvas 100 100 400 300 12;\n#X obj 10 10 t b b;\n#X obj 90 10 print snapshot;\n"
            "#N canvas 0 0 300 200 twin 0;\n#X obj 20 20 print first;\n#N canvas 0 0 200 100 inner 0;\n#X text 10 10 deeper;\n#X restore 50 50 pd inner;\n#X restore 20 80 pd twin;\n"
            "#N canvas 0 0 300 200 twin 0;\n#X obj 30 30 print second;\n#X restore 140 80 pd twin;\n";
        for (int ordinal = 0; ordinal < 440; ++ordinal)
            fixture += "#X text 10 " + String(120 + ordinal * 18) + " snapshot-" + String(ordinal) + "-" + String::repeatedString("x", 96) + ";\n";
        fixture += "#X connect 0 0 1 0;\n#X connect 0 1 1 0;\n";

        auto* canvas = openBoundExportFixture(fixture, "snapshot-root");
        check(canvas != nullptr, "snapshot fixture must open");
        if (canvas) {
            auto sendRead = [this](int id, Array<int> const& path, String const& cursor, String const& revision, bool ok, String const& error) {
                auto* request = new DynamicObject();
                request->setProperty("version", 1);
                request->setProperty("request_id", id);
                request->setProperty("operation", "read_canvas");
                request->setProperty("generation", "snapshot-generation");
                Array<var> pathValue;
                for (auto ordinal : path)
                    pathValue.add(ordinal);
                request->setProperty("canvas_path", var(pathValue));
                if (cursor.isNotEmpty())
                    request->setProperty("cursor", cursor);
                if (revision.isNotEmpty())
                    request->setProperty("expected_revision", revision);
                consoleBegin();
                sendRequest(var(request), id, ok, error);
                flushDebugQueue();
                check(responses.size() == 1, "snapshot read must return one correlated reply");
                checkConsoleReply(0, id, ok, error);
                return responses.isEmpty() ? var() : responses[0];
            };

            consoleBegin();
            sendRequest(makeSetGenerationRequest(700, "snapshot-generation", "snapshot-root"), 700, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 700, true, {});

            Array<var> assembled;
            String revision, cursor;
            int pages = 0;
            int largestPage = 0;
            do {
                auto result = sendRead(701 + pages, {}, cursor, revision, true, {});
                largestPage = jmax(largestPage, static_cast<int>(Base64::toBase64(JSON::toString(result, true)).getNumBytesAsUTF8()));
                auto* response = result.getDynamicObject();
                auto* data = response ? response->getProperty("data").getDynamicObject() : nullptr;
                if (!data) {
                    check(false, "snapshot page must have data");
                    break;
                }
                auto const pageRevision = data->getProperty("revision").toString();
                check(pageRevision.length() == 64, "snapshot revision must be an opaque SHA256 token");
                if (revision.isNotEmpty())
                    check(pageRevision == revision, "all snapshot pages must share one root token");
                revision = pageRevision;
                auto* rows = data->getProperty("records").getArray();
                check(rows && !rows->isEmpty(), "snapshot page must make progress");
                if (rows)
                    assembled.addArray(*rows);
                cursor = data->getProperty("next_cursor").toString();
                ++pages;
            } while (cursor.isNotEmpty() && pages < 20);
            check(pages > 1 && cursor.isEmpty(), "nested snapshot larger than 60 KiB must finish in multiple pages");
            check(largestPage >= 56 * 1024 && largestPage <= maxEncodedResponseBytes,
                "the near-60-KiB page must arrive complete through the real receiver");
            check(assembled.size() == 454, "reassembled snapshot must contain four canvases, 448 objects and two cords");
            if (assembled.size() == 454) {
                auto* rootRecord = assembled[0].getDynamicObject();
                check(rootRecord->getProperty("kind").toString() == "canvas" && rootRecord->getProperty("canvas_path").getArray()->isEmpty(),
                    "root canvas must be first");
                for (int ordinal = 0; ordinal < 444; ++ordinal) {
                    auto* row = assembled[ordinal + 1].getDynamicObject();
                    check(row && static_cast<int>(row->getProperty("ordinal")) == ordinal, "root object ordinals must agree with the Pd fixture");
                    if (ordinal >= 4)
                        check(row->getProperty("text").toString().contains("snapshot-" + String(ordinal - 4))
                            && static_cast<int>(row->getProperty("x")) == 10
                            && static_cast<int>(row->getProperty("y")) == 120 + (ordinal - 4) * 18,
                            "comment text and position must agree with the live Pd model");
                }
                for (int index = 445; index <= 446; ++index) {
                    auto* row = assembled[index].getDynamicObject();
                    check(row && row->getProperty("kind").toString() == "connection"
                        && static_cast<int>(row->getProperty("source")) == 0
                        && static_cast<int>(row->getProperty("target")) == 1
                        && static_cast<int>(row->getProperty("outlet")) == index - 445,
                        "root cords must agree with the Pd model");
                }
                auto* first = assembled[447].getDynamicObject();
                auto* second = assembled[452].getDynamicObject();
                check(first && second && first->getProperty("name").toString() == "twin"
                    && second->getProperty("name").toString() == "twin"
                    && static_cast<int>((*first->getProperty("canvas_path").getArray())[0]) == 2
                    && static_cast<int>((*second->getProperty("canvas_path").getArray())[0]) == 3,
                    "equal-named sibling canvases must have distinct ordinal paths");
            }

            auto child = sendRead(730, { 2 }, {}, revision, true, {});
            auto* childData = child.getDynamicObject() ? child.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            check(childData && childData->getProperty("records").getArray()->size() == 5,
                "scoped read must include child and descendant records only");
            sendRead(731, { 1 }, {}, {}, false, "CanvasTypeMismatch");
            sendRead(732, { 999 }, {}, {}, false, "CanvasNotFound");
            sendRead(733, {}, "forged", revision, false, "InvalidRequest");
            sendRead(734, {}, {}, "stale", false, "StaleRevision");
            sendRead(735, {}, {}, revision, true, {});

            auto first = sendRead(736, {}, {}, {}, true, {});
            auto* firstData = first.getDynamicObject() ? first.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            String const staleCursor = firstData ? firstData->getProperty("next_cursor").toString() : String();
            check(staleCursor.isNotEmpty(), "first page must offer a continuation before external move");
            editor->pd->lockAudioThread();
            auto* root = canvas->patch.getRawPointer();
            auto* childCanvas = reinterpret_cast<t_canvas*>(root->gl_list->g_next->g_next);
            auto* moved = childCanvas->gl_list;
            int const beforeX = reinterpret_cast<t_text*>(moved)->te_xpix;
            editor->pd->unlockAudioThread();
            auto* childGui = editor->getTabComponent().openPatch(pd::Patch::Ptr(new pd::Patch(pd::WeakReference(childCanvas, editor->pd), editor->pd, false)), false, false);
            check(childGui && childGui->isShowing(), "the nested canvas must be displayed before its GUI edit");
            if (childGui) {
                childGui->performSynchronise();
                check(!childGui->objects.empty(), "displayed child must have a movable GUI object");
                if (!childGui->objects.empty()) {
                    auto* guiObject = childGui->objects.front();
                    int const guiBeforeX = guiObject->getX();
                    childGui->locked.setValue(false);
                    childGui->deselectAll();
                    childGui->setSelected(guiObject, true);
                    childGui->keyPressed(KeyPress(KeyPress::rightKey, ModifierKeys::shiftModifier, 0));
                    editor->pd->lockAudioThread();
                    int const afterX = reinterpret_cast<t_text*>(moved)->te_xpix;
                    editor->pd->unlockAudioThread();
                    check(afterX == beforeX + 1 && guiObject->getX() == guiBeforeX + 1,
                        "GUI keyboard edit must move both displayed child object and Pd model");
                    std::cout << "[canvas-snapshot] displayed child keyboard move GUI x=" << guiBeforeX << "->" << guiObject->getX()
                              << " Pd x=" << beforeX << "->" << afterX << std::endl;
                }
            }
            auto stale = sendRead(737, {}, staleCursor, revision, false, "StaleRevision");
            std::cout << "[canvas-snapshot] correlated stale=" << JSON::toString(stale, true) << std::endl;
            auto changed = sendRead(738, {}, {}, {}, true, {});
            auto* changedData = changed.getDynamicObject() ? changed.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            check(changedData && changedData->getProperty("revision").toString() != revision,
                "child movement must change the root-wide revision");
            String const changedRevision = changedData ? changedData->getProperty("revision").toString() : String();
            if (changedData) {
                sendRead(743, { 2 }, changedData->getProperty("next_cursor").toString(), changedRevision, false, "InvalidRequest");
                if (childGui && !childGui->objects.empty()) {
                    childGui->keyPressed(KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0));
                    editor->pd->lockAudioThread();
                    int const revertedX = reinterpret_cast<t_text*>(moved)->te_xpix;
                    editor->pd->unlockAudioThread();
                    check(revertedX == beforeX, "GUI keyboard reverse edit must restore the Pd child position");
                }
                auto reverted = sendRead(744, { 3 }, {}, {}, true, {});
                auto* revertedData = reverted.getDynamicObject() ? reverted.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
                check(revertedData && revertedData->getProperty("revision").toString() == revision,
                    "moving a child back to identical content must restore the same root token");
            }

            editor->pd->lockAudioThread();
            auto* oversizedObject = root->gl_list->g_next->g_next->g_next->g_next;
            auto* oversizedText = reinterpret_cast<t_text*>(oversizedObject);
            auto* originalBinbuf = oversizedText->te_binbuf;
            auto* largeBinbuf = binbuf_new();
            Array<t_atom> words;
            for (int index = 0; index < 320; ++index) {
                t_atom word;
                SETSYMBOL(&word, editor->pd->generateSymbol(String::repeatedString("q", 100) + String(index)));
                words.add(word);
            }
            binbuf_add(largeBinbuf, words.size(), words.getRawDataPointer());
            oversizedText->te_binbuf = largeBinbuf;
            editor->pd->unlockAudioThread();
            auto largeFits = sendRead(742, {}, {}, {}, true, {});
            auto* fitsData = largeFits.getDynamicObject() ? largeFits.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            auto* fitsRecords = fitsData ? fitsData->getProperty("records").getArray() : nullptr;
            check(fitsRecords && fitsRecords->size() >= 6
                && (*fitsRecords)[5].getDynamicObject()->getProperty("text").toString().length() > 32000,
                "one large object record must arrive complete without truncation");
            editor->pd->lockAudioThread();
            words.clear();
            for (int index = 320; index < 500; ++index) {
                t_atom word;
                SETSYMBOL(&word, editor->pd->generateSymbol(String::repeatedString("q", 100) + String(index)));
                words.add(word);
            }
            binbuf_add(largeBinbuf, words.size(), words.getRawDataPointer());
            editor->pd->unlockAudioThread();
            auto largeFirst = sendRead(740, {}, {}, {}, true, {});
            auto* largeData = largeFirst.getDynamicObject() ? largeFirst.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            String const largeCursor = largeData ? largeData->getProperty("next_cursor").toString() : String();
            String const largeRevision = largeData ? largeData->getProperty("revision").toString() : String();
            check(largeCursor.isNotEmpty(), "oversized record must yield a cursor after preceding records");
            if (largeCursor.isNotEmpty())
                sendRead(741, {}, largeCursor, largeRevision, false, "RecordTooLarge");
            std::cout << "[canvas-snapshot] correlated id=741 oversized="
                      << (responses.isEmpty() ? String() : JSON::toString(responses[0], true)) << std::endl;
            editor->pd->lockAudioThread();
            oversizedText->te_binbuf = originalBinbuf;
            binbuf_free(largeBinbuf);
            editor->pd->unlockAudioThread();
            std::cout << "[canvas-snapshot] pages=" << pages << " records=" << assembled.size() << " largest-encoded=" << largestPage
                      << " stale=StaleRevision first=" << JSON::toString(assembled.isEmpty() ? var() : assembled[0], true)
                      << " changed=" << (changedData ? changedData->getProperty("revision").toString() : String()) << std::endl;

            if (childGui)
                editor->getTabComponent().closeTab(childGui);
            std::cout << "[canvas-snapshot] child GUI tab closed" << std::endl;
            closeExportFixture(canvas, "snapshot-root");
            std::cout << "[canvas-snapshot] root fixture closed" << std::endl;

            auto scalarPatch = editor->pd->loadPatch(
                "#N struct snapshot-pos float x float y;\n#N canvas 100 100 300 200 12;\n#X scalar snapshot-pos 37 59 \\;;\n");
            check(scalarPatch != nullptr, "non-text scalar position fixture must load");
            if (scalarPatch) {
                auto* scalarRoot = scalarPatch->getRawPointer();
                editor->pd->lockAudioThread();
                bool const isScalar = scalarRoot->gl_list && pd_class(&scalarRoot->gl_list->g_pd) == scalar_class;
                pd_bind(&scalarRoot->gl_obj.ob_pd, editor->pd->generateSymbol("snapshot-scalar-root"));
                editor->pd->unlockAudioThread();
                check(isScalar, "the position fixture must contain a real non-text Pd scalar");
                consoleBegin();
                sendRequest(makeSetGenerationRequest(750, "snapshot-generation", "snapshot-scalar-root"), 750, true, {});
                flushDebugQueue();
                checkConsoleReply(0, 750, true, {});
                auto scalarReply = sendRead(751, {}, {}, {}, true, {});
                auto* scalarData = scalarReply.getDynamicObject() ? scalarReply.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
                auto* scalarRecords = scalarData ? scalarData->getProperty("records").getArray() : nullptr;
                check(scalarRecords && scalarRecords->size() == 2, "scalar read must include canvas and one non-text record");
                if (scalarRecords && scalarRecords->size() == 2) {
                    auto* scalar = (*scalarRecords)[1].getDynamicObject();
                    check(scalar && scalar->getProperty("coordinate_space").toString() == "data"
                        && scalar->getProperty("template").toString().contains("snapshot-pos")
                        && scalar->getProperty("x").isDouble() && static_cast<double>(scalar->getProperty("x")) == 37.0
                        && scalar->getProperty("y").isDouble() && static_cast<double>(scalar->getProperty("y")) == 59.0,
                        "non-text scalar must report its Pd data coordinates, not a GUI bounding box");
                    std::cout << "[canvas-snapshot] scalar=" << JSON::toString((*scalarRecords)[1], true) << std::endl;
                }
                editor->pd->lockAudioThread();
                pd_unbind(&scalarRoot->gl_obj.ob_pd, editor->pd->generateSymbol("snapshot-scalar-root"));
                editor->pd->unlockAudioThread();
                editor->pd->patches.remove_one(scalarPatch, [](auto const& left, auto const& right) { return *left == *right; });
            }
            consoleBegin();
            sendRequest(makeSetGenerationRequest(739, "generation-1", rootReceiver), 739, true, {});
            flushDebugQueue();
            checkConsoleReply(0, 739, true, {});
        }
        expectedReplies = previousReplies;
        responses = previousResponses;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasPreflightOrderedRefs()
    {
        beginTest("Canvas preflight ordered refs");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const fixture = "#N canvas 100 100 400 300 12;\n"
            "#X obj 10 10 t b b;\n#X obj 100 10 print root;\n#X obj 190 10 sig~ 440;\n"
            "#N canvas 0 0 300 200 nested 0;\n#X obj 20 20 t b b;\n#X obj 90 20 print child;\n"
            "#X connect 0 0 1 0;\n#X restore 20 80 pd nested;\n#X connect 0 0 1 0;\n";
        auto* canvas = openBoundExportFixture(fixture, "preflight-root");
        check(canvas != nullptr, "preflight fixture must open");
        if (!canvas) {
            expectedReplies = previousReplies;
            responses = previousResponses;
            beginTest("Versioned receiver and generation registration");
            return;
        }

        auto sendSetGeneration = [this](int const id, String const& generation, String const& receiver) {
            consoleBegin();
            sendRequest(makeSetGenerationRequest(id, generation, receiver), id, true, {});
            flushDebugQueue();
            checkConsoleReply(0, id, true, {});
        };
        auto readSnapshot = [this](int const id, String const& path, String const& revision, bool const ok, String const& error) {
            String json = "{\"version\":1,\"request_id\":" + String(id)
                + ",\"operation\":\"read_canvas\",\"generation\":\"preflight-generation\",\"canvas_path\":" + path;
            if (revision.isNotEmpty())
                json += ",\"expected_revision\":\"" + revision + "\"";
            json += "}";
            consoleBegin();
            sendJson(json, id, ok, error);
            flushDebugQueue();
            check(responses.size() == 1, "preflight snapshot must receive one reply for request " + String(id));
            checkConsoleReply(0, id, ok, error);
            return responses.isEmpty() ? var() : responses[0];
        };
        auto sendEdit = [this](int const id, String const& generation, String const& revision,
                            String const& edits, String const& expectedError) {
            String const json = "{\"version\":1,\"request_id\":" + String(id)
                + ",\"operation\":\"edit_canvas\",\"generation\":\"" + generation
                + "\",\"expected_revision\":\"" + revision + "\",\"edits\":" + edits + "}";
            var request;
            auto const parseResult = JSON::parse(json, request);
            check(parseResult.wasOk() && request.isObject(), "preflight request fixture must parse as a JSON object");
            String const wireJson = JSON::toString(request, true);
            consoleBegin();
            sendRequest(request, id, expectedError.isEmpty(), expectedError);
            flushDebugQueue();
            check(responses.size() == 1, "edit preflight must receive one reply for request " + String(id));
            checkConsoleReply(0, id, expectedError.isEmpty(), expectedError);
            auto* response = responses.isEmpty() ? nullptr : responses[0].getDynamicObject();
            if (expectedError == "NotYetAvailable")
                check(response && !response->hasProperty("data"), "preflight-valid edit must not report applied data");
            if (response)
                std::cout << "[canvas-preflight] id=" << id << " request=" << wireJson
                          << " encoded-request=" << Base64::toBase64(wireJson.toRawUTF8(), wireJson.getNumBytesAsUTF8())
                          << " reply=" << JSON::toString(responses[0], true) << std::endl;
            return responses.isEmpty() ? var() : responses[0];
        };
        auto recordsOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? JSON::toString(data->getProperty("records"), true) : String();
        };
        auto revisionOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };

        sendSetGeneration(800, "preflight-generation", "preflight-root");
        var const initialRoot = readSnapshot(801, "[]", {}, true, {});
        String const initialRevision = revisionOf(initialRoot);
        String const initialRootBytes = recordsOf(initialRoot);
        var const initialChild = readSnapshot(802, "[3]", initialRevision, true, {});
        String const initialChildBytes = recordsOf(initialChild);
        check(initialRevision.length() == 64, "preflight fixture must provide a root-wide revision");
        check(initialRootBytes.isNotEmpty() && initialChildBytes.isNotEmpty(), "root and nested snapshots must be present");

        int snapshotId = 804;
        auto checkUnchanged = [&](String const& expectedRevision, String const& expectedRoot, String const& expectedChild, String const& label) {
            auto const currentRoot = readSnapshot(snapshotId++, "[]", expectedRevision, true, {});
            auto const currentChild = readSnapshot(snapshotId++, "[3]", expectedRevision, true, {});
            bool const sameRoot = recordsOf(currentRoot) == expectedRoot;
            bool const sameChild = recordsOf(currentChild) == expectedChild;
            check(sameRoot, label + " must preserve byte-identical root records");
            check(sameChild, label + " must preserve byte-identical child records");
            check(revisionOf(currentRoot) == expectedRevision, label + " must preserve the root revision");
            if (label == "valid root add plus invalid child port" || label == "stale GUI precondition") {
                std::cout << "[canvas-preflight] unchanged=" << label
                          << " root-bytes=" << expectedRoot.getNumBytesAsUTF8()
                          << " root-before=" << expectedRoot
                          << " root-after=" << recordsOf(currentRoot)
                          << " child-bytes=" << expectedChild.getNumBytesAsUTF8()
                          << " child-before=" << expectedChild
                          << " child-after=" << recordsOf(currentChild)
                          << " byte-equal=" << (sameRoot && sameChild) << std::endl;
            }
        };

        String const connectBarrier = "{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0},"
            "{\"op\":\"disconnect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0}";
        sendEdit(803, "preflight-generation", initialRevision,
            "[" + connectBarrier + ","
            "{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":10,\"y\":10},"
            "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"new-child\",\"type\":\"subpatch\",\"x\":240,\"y\":130,\"name\":\"planned\"},"
            "{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"new-child\"}],\"ref\":\"new-box\",\"type\":\"object\",\"x\":20,\"y\":20,\"text\":\"print planned\"},"
            "{\"op\":\"move\",\"canvas_path\":[{\"ref\":\"new-child\"}],\"target\":{\"ref\":\"new-box\"},\"x\":40,\"y\":40},"
            "{\"op\":\"move\",\"canvas_path\":[],\"target\":99,\"x\":1,\"y\":1}]", "ObjectNotFound");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "ordered ref projection");

        sendEdit(806, "preflight-generation", initialRevision,
            "[" + connectBarrier + ","
            "{\"op\":\"remove\",\"canvas_path\":[],\"target\":0},"
            "{\"op\":\"move\",\"canvas_path\":[],\"target\":1,\"x\":30,\"y\":30},"
            "{\"op\":\"move\",\"canvas_path\":[3],\"target\":0,\"x\":40,\"y\":40},"
            "{\"op\":\"move\",\"canvas_path\":[],\"target\":99,\"x\":1,\"y\":1}]", "ObjectNotFound");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "pre-state ordinal projection");

        sendEdit(809, "preflight-generation", initialRevision,
            "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"pending-root\",\"type\":\"object\",\"x\":240,\"y\":130,\"text\":\"print pending\"},"
            "{\"op\":\"connect\",\"canvas_path\":[3],\"source\":0,\"outlet\":0,\"target\":1,\"inlet\":1}]", "InvalidPort");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "valid root add plus invalid child port");

        sendEdit(812, "preflight-generation", initialRevision,
            "[{\"op\":\"connect\",\"canvas_path\":[3],\"source\":0,\"outlet\":-1,\"target\":1,\"inlet\":0}]", "InvalidRequest");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "negative port syntax");
        sendEdit(815, "preflight-generation", initialRevision,
            "[{\"op\":\"connect\",\"canvas_path\":[3],\"source\":0,\"outlet\":2,\"target\":1,\"inlet\":0}]", "InvalidPort");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "out-of-range port");
        sendEdit(818, "preflight-generation", initialRevision,
            "[{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":0,\"target\":1,\"inlet\":0}]", "InvalidConnection");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "duplicate existing edge");
        sendEdit(821, "preflight-generation", initialRevision,
            "[{\"op\":\"disconnect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0}]", "InvalidConnection");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "absent existing edge");
        sendEdit(824, "preflight-generation", initialRevision,
            "[{\"op\":\"connect\",\"canvas_path\":[],\"source\":2,\"outlet\":0,\"target\":1,\"inlet\":0}]", "InvalidConnection");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "signal type mismatch");
        sendEdit(827, "preflight-generation", initialRevision,
            "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"cross-child\",\"type\":\"subpatch\",\"x\":240,\"y\":130,\"name\":\"cross\"},"
            "{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"cross-child\"}],\"ref\":\"inside\",\"type\":\"object\",\"x\":20,\"y\":20,\"text\":\"print inside\"},"
            "{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":0,\"target\":{\"ref\":\"inside\"},\"inlet\":0}]", "InvalidConnection");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "cross-canvas ref endpoint");
        sendEdit(830, "preflight-generation", initialRevision,
            "[{\"op\":\"remove\",\"canvas_path\":[],\"target\":3},"
            "{\"op\":\"move\",\"canvas_path\":[3],\"target\":0,\"x\":50,\"y\":50}]", "CanvasNotFound");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "deleted ancestor path");
        sendEdit(833, "preflight-generation", initialRevision,
            "[{\"op\":\"remove\",\"canvas_path\":[],\"target\":0},"
            "{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":50,\"y\":50}]", "ObjectNotFound");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "deleted object target");
        sendEdit(836, "preflight-generation", initialRevision,
            "[{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0},"
            "{\"op\":\"disconnect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0}]", "");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "projected connect then disconnect");
        sendEdit(839, "preflight-generation", initialRevision,
            "[{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0},"
            "{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":1,\"inlet\":0}]", "InvalidConnection");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "projected duplicate edge");
        sendEdit(842, "preflight-generation", initialRevision,
            "[{\"op\":\"disconnect\",\"canvas_path\":[],\"source\":0,\"outlet\":0,\"target\":1,\"inlet\":0},"
            "{\"op\":\"disconnect\",\"canvas_path\":[],\"source\":0,\"outlet\":0,\"target\":1,\"inlet\":0}]", "InvalidConnection");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "projected absent edge");
        sendEdit(845, "preflight-generation", initialRevision,
            "[{\"op\":\"move\",\"canvas_path\":[99],\"target\":0,\"x\":10,\"y\":10}]", "CanvasNotFound");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "missing child");
        sendEdit(848, "preflight-generation", initialRevision,
            "[{\"op\":\"move\",\"canvas_path\":[0],\"target\":0,\"x\":10,\"y\":10}]", "CanvasTypeMismatch");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "non-canvas path");
        sendEdit(851, "preflight-generation", initialRevision,
            "[{\"op\":\"move\",\"canvas_path\":[],\"target\":99,\"x\":10,\"y\":10}]", "ObjectNotFound");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "missing object");
        sendEdit(854, "stale-generation", initialRevision,
            "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":10,\"y\":10}]", "StaleGeneration");
        checkUnchanged(initialRevision, initialRootBytes, initialChildBytes, "stale generation");

        t_canvas* childPd = nullptr;
        t_text* childObject = nullptr;
        editor->pd->lockAudioThread();
        auto* rootPd = canvas->patch.getRawPointer();
        auto* childBox = rootPd->gl_list;
        for (int ordinal = 0; childBox && ordinal < 3; ++ordinal)
            childBox = childBox->g_next;
        if (childBox && pd_class(&childBox->g_pd) == canvas_class) {
            childPd = reinterpret_cast<t_canvas*>(childBox);
            childObject = pd::Interface::checkObject(childPd->gl_list);
        }
        editor->pd->unlockAudioThread();
        check(childPd != nullptr && childObject != nullptr, "the nested GUI fixture must resolve under the Pd lock");
        Canvas* childGui = childPd
            ? editor->getTabComponent().openPatch(pd::Patch::Ptr(new pd::Patch(pd::WeakReference(childPd, editor->pd), editor->pd, false)), false, false)
            : nullptr;
        check(childGui && childGui->isShowing(), "the nested precondition fixture must be displayed");
        if (childGui) {
            childGui->performSynchronise();
            check(!childGui->objects.empty(), "the displayed nested fixture must contain an object");
            if (!childGui->objects.empty()) {
                auto* guiObject = childGui->objects.front();
                int const guiBeforeX = guiObject->getX();
                editor->pd->lockAudioThread();
                int const pdBeforeX = childObject->te_xpix;
                editor->pd->unlockAudioThread();
                childGui->locked.setValue(false);
                childGui->deselectAll();
                childGui->setSelected(guiObject, true);
                childGui->keyPressed(KeyPress(KeyPress::rightKey, ModifierKeys::shiftModifier, 0));
                editor->pd->lockAudioThread();
                int const pdAfterX = childObject->te_xpix;
                editor->pd->unlockAudioThread();
                check(pdAfterX == pdBeforeX + 1 && guiObject->getX() == guiBeforeX + 1,
                    "a real GUI keyboard edit must change both the Pd model and displayed child");
                std::cout << "[canvas-preflight] stale-GUI move pd-x=" << pdBeforeX << "->" << pdAfterX
                          << " gui-x=" << guiBeforeX << "->" << guiObject->getX() << std::endl;

                auto const changedRoot = readSnapshot(857, "[]", {}, true, {});
                String const changedRevision = revisionOf(changedRoot);
                String const changedRootBytes = recordsOf(changedRoot);
                auto const changedChild = readSnapshot(858, "[3]", changedRevision, true, {});
                String const changedChildBytes = recordsOf(changedChild);
                check(changedRevision != initialRevision, "the GUI edit must stale the original root-wide revision");
                sendEdit(859, "preflight-generation", initialRevision,
                    "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":10,\"y\":10}]", "StaleRevision");
                checkUnchanged(changedRevision, changedRootBytes, changedChildBytes, "stale GUI precondition");
                sendEdit(862, "stale-generation", changedRevision,
                    "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":10,\"y\":10}]", "StaleGeneration");
                checkUnchanged(changedRevision, changedRootBytes, changedChildBytes, "stale generation after GUI edit");
                childGui->keyPressed(KeyPress(KeyPress::leftKey, ModifierKeys::shiftModifier, 0));
                editor->pd->lockAudioThread();
                int const pdRestoredX = childObject->te_xpix;
                editor->pd->unlockAudioThread();
                check(pdRestoredX == pdBeforeX, "the manual GUI edit must be restored before fixture cleanup");
                auto const restoredRoot = readSnapshot(865, "[]", {}, true, {});
                check(revisionOf(restoredRoot) == initialRevision, "reverting the GUI edit must restore the initial revision");
            }
            editor->getTabComponent().closeTab(childGui);
        }

        closeExportFixture(canvas, "preflight-root");
        sendSetGeneration(866, "generation-1", rootReceiver);
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-preflight] root and child fixtures closed; initial snapshot records remained unchanged after all rejected batches"
                  << std::endl;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasAcknowledgmentAndMapping()
    {
        beginTest("Canvas acknowledgment and mapping");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const fixture = "#N canvas 100 100 400 300 12;\n#X obj 20 20 t b b;\n#X obj 120 20 print root;\n"
            "#N canvas 0 0 240 180 child 0;\n#X obj 20 20 print nested;\n#X restore 50 90 pd child;\n"
            "#X connect 0 0 1 0;\n";
        auto* root = openBoundExportFixture(fixture, "ack-root");
        check(root != nullptr, "acknowledgment fixture must open");
        if (!root)
            return;
        auto send = [&](int id, String const& operation, bool ok, String const& error = String()) {
            String const request = "{\"version\":1,\"request_id\":" + String(id) + ",\"generation\":\"ack-generation\"," + operation + "}";
            consoleBegin();
            sendJson(request, id, ok, error);
            flushDebugQueue();
            check(responses.size() == 1, "acknowledgment request must return exactly one reply");
            checkConsoleReply(0, id, ok, error);
            auto const result = responses.isEmpty() ? var() : responses[0];
            if (id < 1520 || id == 1563 || id == 1565 || id == 1568 || id == 1573)
                std::cout << "[canvas-ack] request=" << request << " reply=" << JSON::toString(result, true) << std::endl;
            return result;
        };
        auto revisionOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };
        auto read = [&](int id) {
            return send(id, "\"operation\":\"read_canvas\",\"canvas_path\":[]", true);
        };
        auto edit = [&](int id, String const& revision, String const& edits, bool ok, String const& error = String()) {
            return send(id, "\"operation\":\"edit_canvas\",\"expected_revision\":\"" + revision + "\",\"edits\":" + edits, ok, error);
        };
        send(1500, "\"operation\":\"set_generation\",\"root_receiver\":\"ack-root\"", true);
        auto const initialRevision = revisionOf(read(1501));
        pd::Patch::Ptr childPatch;
        editor->pd->lockAudioThread();
        auto* childObject = root->patch.getRawPointer()->gl_list->g_next->g_next;
        if (childObject && pd_class(&childObject->g_pd) == canvas_class)
            childPatch = new pd::Patch(pd::WeakReference(reinterpret_cast<t_canvas*>(childObject), editor->pd), editor->pd, false);
        editor->pd->unlockAudioThread();
        auto* child = childPatch ? editor->getTabComponent().openPatch(childPatch, false, false) : nullptr;
        check(child != nullptr, "nested canvas must be displayed before edit acknowledgment");
        if (child) {
            editor->getTabComponent().showTab(root, 0);
            editor->getTabComponent().showTab(child, 1);
        }
        auto applied = edit(1502, initialRevision,
            "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":35,\"y\":35},"
            "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"new\",\"type\":\"object\",\"x\":210,\"y\":45,\"text\":\"print new\"},"
            "{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":1,\"target\":{\"ref\":\"new\"},\"inlet\":0},"
            "{\"op\":\"move\",\"canvas_path\":[2],\"target\":0,\"x\":45,\"y\":50}]", true);
        auto* data = applied.getDynamicObject() ? applied.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
        auto* mappings = data ? data->getProperty("mappings").getDynamicObject() : nullptr;
        auto* refs = mappings ? mappings->getProperty("refs").getDynamicObject() : nullptr;
        auto* added = refs ? refs->getProperty("new").getDynamicObject() : nullptr;
        String const appliedRevision = data ? data->getProperty("revision").toString() : String();
        auto const appliedRead = read(1503);
        check(data && data->getProperty("status").toString() == "applied" && appliedRevision != initialRevision
                && appliedRevision == revisionOf(appliedRead) && added
                && static_cast<int>(added->getProperty("ordinal")) == 3,
            "applied reply must bind native revision and usable added object identity");
        check(root->objects.size() == 4 && root->connections.size() == 2
                && root->objects[0]->getObjectBounds().getPosition() == Point<int>(35, 35),
            "displayed root must contain the new object, wire, and moved position before reply");
        check(child && child->objects.size() == 1 && child->objects[0]->getObjectBounds().getPosition() == Point<int>(45, 50),
            "displayed nested/split canvas must be moved before reply");
        String const saved = root->patch.getCanvasContent();
        check(saved.contains("#X obj 35 35 t b b;") && saved.contains("#X connect 0 1 3 0;")
                && saved.contains("#X obj 45 50 print nested;"),
            "post-ack Pd serialization must retain root and nested positions and the new cord");
        std::cout << "[canvas-ack] gui-root=4/2 at=35,35 gui-child=1 at=45,50 saved-root="
                  << saved.contains("#X obj 35 35 t b b;") << " saved-cord="
                  << saved.contains("#X connect 0 1 3 0;") << " saved-child="
                  << saved.contains("#X obj 45 50 print nested;") << std::endl;
        auto next = edit(1504, appliedRevision,
            "[{\"op\":\"move\",\"canvas_path\":[],\"target\":3,\"x\":220,\"y\":55}]", true);
        check(next.getDynamicObject() && next.getDynamicObject()->getProperty("data").isObject(),
            "returned mapping must address the next successful edit");
        pd::setCanvasEditGuiMismatchHookForTesting(breakAcknowledgmentCanvas, root);
        auto mismatched = edit(1509, revisionOf(read(1510)),
            "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":40,\"y\":40}]", false, "PartialOrUncertain");
        pd::setCanvasEditGuiMismatchHookForTesting(nullptr, nullptr);
        auto* mismatchedError = mismatched.getDynamicObject() ? mismatched.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
        check(mismatchedError && mismatchedError->getProperty("revision").toString() == revisionOf(read(1511))
                && root->objects.size() == 3,
            "native success with a mismatched displayed canvas must not be acknowledged");
        root->performSynchronise();
        bool injected = false;
        pd::setCanvasEditFailureHookForTesting(failAfterFirstCanvasAdd, &injected);
        auto partial = edit(1506, revisionOf(read(1507)),
            "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"partial\",\"type\":\"comment\",\"x\":200,\"y\":100,\"text\":\"partial\"},"
            "{\"op\":\"move\",\"canvas_path\":[2],\"target\":0,\"x\":60,\"y\":60}]", false, "PartialOrUncertain");
        pd::setCanvasEditFailureHookForTesting(nullptr, nullptr);
        auto* partialError = partial.getDynamicObject() ? partial.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
        auto partialRead = read(1508);
        auto* recovery = partialError ? partialError->getProperty("recovery").getDynamicObject() : nullptr;
        check(injected && partialError && partialError->getProperty("revision").toString() == revisionOf(partialRead)
                && recovery && recovery->getProperty("expected_revision").toString() == revisionOf(partialRead)
                && root->patch.getCanvasContent().contains("#X text 200 100 partial;")
                && !root->patch.getCanvasContent().contains("#X obj 60 60 print nested;"),
            "injected post-add failure must return current revision, recovery and no replay of later entry");
        if (child)
            editor->getTabComponent().closeTab(child, false);
        closeExportFixture(root, "ack-root");
        String const graphFixture = "#N canvas 100 100 400 300 12;\n"
            "#N canvas 0 0 200 140 (subpatch) 0;\n#X obj 20 20 print graph;\n"
            "#X coords 0 1 100 -1 120 80 1 0 0;\n#X restore 30 30 graph;\n";
        auto* graphRoot = openBoundExportFixture(graphFixture, "ack-graph");
        check(graphRoot != nullptr, "embedded graph fixture must open");
        if (graphRoot) {
            auto findGraph = [&](auto&& self, Component* component) -> Canvas* {
                for (int index = 0; index < component->getNumChildComponents(); ++index) {
                    auto* childComponent = component->getChildComponent(index);
                    if (auto* graphCanvas = dynamic_cast<Canvas*>(childComponent); graphCanvas && graphCanvas->isGraph)
                        return graphCanvas;
                    if (auto* graphCanvas = self(self, childComponent))
                        return graphCanvas;
                }
                return nullptr;
            };
            send(1512, "\"operation\":\"set_generation\",\"root_receiver\":\"ack-graph\"", true);
            auto graphResult = edit(1514, revisionOf(read(1513)),
                "[{\"op\":\"move\",\"canvas_path\":[0],\"target\":0,\"x\":45,\"y\":50}]", true);
            auto* graphCanvas = findGraph(findGraph, graphRoot);
            check(graphResult.getDynamicObject() && graphResult.getDynamicObject()->getProperty("data").isObject()
                    && graphCanvas && graphCanvas->objects.size() == 1 && !graphCanvas->objects[0]->isVisible()
                    && graphRoot->patch.getCanvasContent().contains("#X obj 45 50 print graph;"),
                "embedded graph GUI must converge before root acknowledgment");
            if (graphCanvas) {
                pd::setCanvasEditGuiMismatchHookForTesting(breakAcknowledgmentCanvas, graphCanvas);
                auto graphFailure = edit(1515, revisionOf(read(1516)),
                    "[{\"op\":\"move\",\"canvas_path\":[0],\"target\":0,\"x\":55,\"y\":60}]", false, "PartialOrUncertain");
                pd::setCanvasEditGuiMismatchHookForTesting(nullptr, nullptr);
                auto* failure = graphFailure.getDynamicObject() ? graphFailure.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
                check(failure && failure->getProperty("revision").toString() == revisionOf(read(1517)),
                    "mismatched displayed graph child must prevent applied acknowledgment");
            }
            closeExportFixture(graphRoot, "ack-graph");
        }
        String const pluginFixtureText = "#N canvas 100 100 400 300 12;\n"
            "#X obj 20 20 tgl 15 0 empty empty empty 17 7 0 10 #fcfcfc #000000 #000000 0 1;\n"
            "#X obj 120 20 print root;\n#X connect 0 0 1 0;\n";
        auto* pluginFixture = openBoundExportFixture(pluginFixtureText, "ack-plugin");
        check(pluginFixture != nullptr, "plugin-mode fixture must open");
        if (pluginFixture) {
            auto const editorsBefore = editor->pd->getEditors().size();
            auto pluginPatch = pluginFixture->refCountedPatch;
            auto* relocated = editor->getTabComponent().createNewWindow(pluginFixture);
            auto* modeEditor = relocated ? relocated->editor : nullptr;
            check(modeEditor != nullptr, "plugin-mode fixture must have an isolated editor");
            if (modeEditor) {
                modeEditor->getTabComponent().openInPluginMode(pluginPatch);
                modeEditor->getTabComponent().updateNow();
                auto* pluginCanvas = modeEditor->getPluginModeCanvas();
                check(pluginCanvas && pluginCanvas->isShowing() && modeEditor->getCanvases().empty(),
                    "real plugin mode must display its own canvas with no tab canvases");
                send(1561, "\"operation\":\"set_generation\",\"root_receiver\":\"ack-plugin\"", true);
                auto const modeRevision = revisionOf(read(1562));
                auto modeApplied = edit(1563, modeRevision,
                    "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":35,\"y\":35},"
                    "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"plugin-new\",\"type\":\"object\",\"x\":210,\"y\":45,\"text\":\"print plugin\"},"
                    "{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":0,\"target\":{\"ref\":\"plugin-new\"},\"inlet\":0}]", true);
                auto* modeData = modeApplied.getDynamicObject() ? modeApplied.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
                check(modeData && modeData->getProperty("status").toString() == "applied"
                        && modeData->getProperty("revision").toString() == revisionOf(read(1564))
                        && pluginCanvas && pluginCanvas->objects.size() == 3 && pluginCanvas->connections.size() == 0
                        && !pluginCanvas->connectionLayer.isVisible() && pluginCanvas->objects[0]->isVisible()
                        && pluginCanvas->objects[0]->getObjectBounds().getPosition() == Point<int>(35, 35)
                        && pluginPatch->getCanvasContent().contains("#X connect 0 0 2 0;"),
                    "plugin-mode root GUI objects, cords and position must converge before applied");
                std::cout << "[canvas-ack] plugin-mode gui-count=" << (pluginCanvas ? pluginCanvas->objects.size() : -1)
                          << " gui-cords=" << (pluginCanvas ? pluginCanvas->connections.size() : -1)
                          << " native-cords=" << pluginPatch->getCanvasContent().contains("#X connect 0 0 2 0;")
                          << " native-revision=" << (modeData ? modeData->getProperty("revision").toString() : String()) << std::endl;
                if (pluginCanvas) {
                    pd::setCanvasEditGuiMismatchHookForTesting(breakAcknowledgmentCanvas, pluginCanvas);
                    auto modeMismatch = edit(1565, revisionOf(read(1566)),
                        "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":40,\"y\":40}]", false, "PartialOrUncertain");
                    pd::setCanvasEditGuiMismatchHookForTesting(nullptr, nullptr);
                    auto* mismatchError = modeMismatch.getDynamicObject() ? modeMismatch.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
                    check(mismatchError && mismatchError->getProperty("revision").toString() == revisionOf(read(1567)),
                        "plugin-mode GUI mismatch must never return applied");
                    pluginCanvas->performSynchronise();
                    pd::setCanvasEditGuiMismatchHookForTesting(leavePluginModeDuringCanvasAck, modeEditor);
                    auto modeChanged = edit(1568, revisionOf(read(1569)),
                        "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":45,\"y\":45}]", false, "PartialOrUncertain");
                    pd::setCanvasEditGuiMismatchHookForTesting(nullptr, nullptr);
                    auto* changedError = modeChanged.getDynamicObject() ? modeChanged.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
                    check(changedError && changedError->getProperty("revision").toString() == revisionOf(read(1570))
                            && !modeEditor->isInPluginMode(),
                        "switching from plugin mode during reconciliation must return uncertainty");
                }
                Canvas* restoredTab = nullptr;
                for (auto* canvas : modeEditor->getCanvases())
                    if (canvas->refCountedPatch == pluginPatch)
                        restoredTab = canvas;
                check(restoredTab != nullptr, "plugin-mode fixture must restore its tab before closing");
                if (restoredTab) {
                    editor->pd->lockAudioThread();
                    if (auto* native = pluginPatch->getRawPointer())
                        pd_unbind(&native->gl_obj.ob_pd, editor->pd->generateSymbol("ack-plugin"));
                    editor->pd->unlockAudioThread();
                    modeEditor->getTabComponent().closeTab(restoredTab);
                }
                if (auto* window = dynamic_cast<PlugDataWindow*>(modeEditor->getTopLevelComponent()))
                    window->closeButtonPressed();
                check(editor->pd->getEditors().size() == editorsBefore,
                    "isolated plugin-mode editor must close without replacing the test root editor");
            }
            std::cout << "[canvas-ack] plugin-mode fixture closed" << std::endl;
        }
        String largeFixture = "#N canvas 100 100 400 300 12;\n#N canvas 0 0 240 180 child 0;\n";
        for (int index = 0; index < 1250; ++index)
            largeFixture += "#X obj 10 " + String(index * 20 + 20) + " print entry" + String(index) + ";\n";
        largeFixture += "#X restore 40 40 pd child;\n";
        auto* largeRoot = openBoundExportFixture(largeFixture, "ack-large");
        check(largeRoot != nullptr, "large mapping fixture must open");
        if (largeRoot) {
            send(1520, "\"operation\":\"set_generation\",\"root_receiver\":\"ack-large\"", true);
            auto const largeRevision = revisionOf(read(1521));
            auto largeEdit = edit(1522, largeRevision,
                "[{\"op\":\"move\",\"canvas_path\":[0],\"target\":0,\"x\":60,\"y\":60}]", true);
            auto* largeData = largeEdit.getDynamicObject() ? largeEdit.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            auto const changedRevision = largeData ? largeData->getProperty("revision").toString() : String();
            auto cursor = largeData ? largeData->getProperty("next_cursor").toString() : String();
            String const firstMappingCursor = cursor;
            int mapped = 0, pages = 0;
            while (cursor.isNotEmpty() && pages < 20) {
                auto page = send(1523 + pages,
                    "\"operation\":\"read_canvas\",\"canvas_path\":[],\"expected_revision\":\"" + changedRevision
                        + "\",\"cursor\":\"" + cursor + "\"", true);
                auto* pageData = page.getDynamicObject() ? page.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
                auto* entries = pageData ? pageData->getProperty("records").getArray() : nullptr;
                mapped += entries ? entries->size() : 0;
                cursor = pageData ? pageData->getProperty("next_cursor").toString() : String();
                ++pages;
            }
            check(largeData && largeData->getProperty("status").toString() == "applied" && pages > 1
                    && cursor.isEmpty() && mapped >= 1250 && changedRevision == revisionOf(read(1544)),
                "oversized mappings must be completely pageable at their applied revision");
            auto diverged = edit(1571, changedRevision,
                "[{\"op\":\"move\",\"canvas_path\":[0],\"target\":0,\"x\":80,\"y\":80}]", true);
            auto const divergedRevision = revisionOf(diverged);
            auto restored = edit(1572, divergedRevision,
                "[{\"op\":\"move\",\"canvas_path\":[0],\"target\":0,\"x\":60,\"y\":60}]", true);
            auto* restoredData = restored.getDynamicObject() ? restored.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            auto const restoredCursor = restoredData ? restoredData->getProperty("next_cursor").toString() : String();
            check(divergedRevision != changedRevision && restoredData
                    && restoredData->getProperty("revision").toString() == changedRevision
                    && restoredCursor.isNotEmpty() && restoredCursor != firstMappingCursor,
                "mapping cursor must distinguish A-to-C and B-to-C despite the same resulting revision");
            auto oldCursor = send(1573, "\"operation\":\"read_canvas\",\"canvas_path\":[],\"expected_revision\":\""
                    + changedRevision + "\",\"cursor\":\"" + firstMappingCursor + "\"", false, "StaleRevision");
            auto* oldCursorError = oldCursor.getDynamicObject() ? oldCursor.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
            check(oldCursorError && oldCursorError->getProperty("code").toString() == "StaleRevision",
                "old A-to-C map cursor must be stale after C-to-B-to-C cycle");
            auto freshCursor = send(1574, "\"operation\":\"read_canvas\",\"canvas_path\":[],\"expected_revision\":\""
                    + changedRevision + "\",\"cursor\":\"" + restoredCursor + "\"", true);
            auto* freshData = freshCursor.getDynamicObject() ? freshCursor.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            check(freshData && freshData->getProperty("revision").toString() == changedRevision
                    && freshData->getProperty("records").getArray() && !freshData->getProperty("records").getArray()->isEmpty(),
                "new B-to-C cursor must remain readable at its own post-revision");
            std::cout << "[canvas-ack] mapping cycle A-C=" << changedRevision << " C-B=" << divergedRevision
                      << " B-C=" << restoredData->getProperty("revision").toString()
                      << " old-cursor-ok=" << static_cast<bool>(oldCursor.getDynamicObject()->getProperty("ok"))
                      << " old-cursor-error=" << (oldCursorError ? oldCursorError->getProperty("code").toString() : String()) << std::endl;
            std::cout << "[canvas-ack] mapping pages=" << pages << " entries=" << mapped
                      << " revision=" << changedRevision << std::endl;
            pd::Patch::Ptr hiddenChildPatch;
            editor->pd->lockAudioThread();
            if (auto* childObject = largeRoot->patch.getRawPointer()->gl_list)
                hiddenChildPatch = new pd::Patch(pd::WeakReference(reinterpret_cast<t_canvas*>(childObject), editor->pd), editor->pd, false);
            editor->pd->unlockAudioThread();
            auto* openedChild = hiddenChildPatch ? editor->getTabComponent().openPatch(hiddenChildPatch, false, false) : nullptr;
            check(openedChild && openedChild->objects.size() == 1250
                    && openedChild->objects[0]->getObjectBounds().getPosition() == Point<int>(60, 60),
                "unshown nested canvas must load authoritative Pd positions when opened later");
            if (openedChild)
                editor->getTabComponent().closeTab(openedChild, false);
            bool failureInjected = false;
            pd::setCanvasEditFailureHookForTesting(failAfterFirstCanvasAdd, &failureInjected);
            auto failed = edit(1545, changedRevision,
                "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"partial\",\"type\":\"comment\",\"x\":200,\"y\":100,\"text\":\"partial\"},"
                "{\"op\":\"move\",\"canvas_path\":[0],\"target\":1,\"x\":80,\"y\":80}]", false, "PartialOrUncertain");
            pd::setCanvasEditFailureHookForTesting(nullptr, nullptr);
            auto* failure = failed.getDynamicObject() ? failed.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
            auto const recoveryRevision = failure ? failure->getProperty("revision").toString() : String();
            auto recoveryPage = read(1546);
            auto* recoveryData = recoveryPage.getDynamicObject() ? recoveryPage.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            auto recoveryCursor = recoveryData ? recoveryData->getProperty("next_cursor").toString() : String();
            int recovered = recoveryData && recoveryData->getProperty("records").getArray()
                ? recoveryData->getProperty("records").getArray()->size() : 0;
            int recoveryPages = 1;
            while (recoveryCursor.isNotEmpty() && recoveryPages < 20) {
                auto page = send(1546 + recoveryPages,
                    "\"operation\":\"read_canvas\",\"canvas_path\":[],\"expected_revision\":\"" + recoveryRevision
                        + "\",\"cursor\":\"" + recoveryCursor + "\"", true);
                auto* pageData = page.getDynamicObject() ? page.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
                auto* entries = pageData ? pageData->getProperty("records").getArray() : nullptr;
                recovered += entries ? entries->size() : 0;
                recoveryCursor = pageData ? pageData->getProperty("next_cursor").toString() : String();
                ++recoveryPages;
            }
            check(failureInjected && failure && recoveryRevision != changedRevision
                    && recoveryData && recoveryData->getProperty("revision").toString() == recoveryRevision
                    && recoveryPages > 1 && recoveryCursor.isEmpty() && recovered == 1254,
                "post-dispatch failure must expose a complete revision-bound paged readback");
            if (firstMappingCursor.isNotEmpty())
                send(1560, "\"operation\":\"read_canvas\",\"canvas_path\":[],\"expected_revision\":\""
                        + recoveryRevision + "\",\"cursor\":\"" + firstMappingCursor + "\"", false, "StaleRevision");
            std::cout << "[canvas-ack] recovery pages=" << recoveryPages << " records=" << recovered
                      << " revision=" << recoveryRevision << " error=" << JSON::toString(failed, true) << std::endl;
            closeExportFixture(largeRoot, "ack-large");
        }
        registerExportGeneration(1505, "generation-1", rootReceiver);
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-ack] fixture closed" << std::endl;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasOrderedConnectAndDisconnect()
    {
        beginTest("Ordered connect and disconnect");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const fixture = "#N canvas 100 100 400 300 12;\n"
            "#X obj 10 10 t b b;\n#X obj 100 10 print root;\n#X obj 190 10 sig~ 440;\n"
            "#N canvas 0 0 240 180 nested 0;\n#X obj 10 10 t b b;\n"
            "#X obj 100 10 print child;\n#X obj 100 50 print spare;\n"
            "#X connect 0 0 1 0;\n#X restore 20 80 pd nested;\n"
            "#X connect 0 0 1 0 wire-route;\n";
        auto* canvas = openBoundExportFixture(fixture, "wire-root");
        check(canvas != nullptr, "wire fixture must open");
        if (!canvas) {
            expectedReplies = previousReplies;
            responses = previousResponses;
            beginTest("Versioned receiver and generation registration");
            return;
        }
        String wireGeneration = "wire-generation";
        auto send = [this](int id, String const& body, bool ok, String const& expectedError) {
            String const json = "{\"version\":1,\"request_id\":" + String(id) + "," + body + "}";
            var request;
            check(JSON::parse(json, request).wasOk(), "wire request must parse");
            consoleBegin();
            sendRequest(request, id, ok, expectedError);
            flushDebugQueue();
            check(responses.size() == 1, "wire request must have one reply");
            checkConsoleReply(0, id, ok, expectedError);
            var result = responses.isEmpty() ? var() : responses[0];
            std::cout << "[canvas-wire] id=" << id << " request=" << json
                      << " reply=" << JSON::toString(result, true) << std::endl;
            return result;
        };
        auto read = [&](int id, String const& path) {
            return send(id, "\"operation\":\"read_canvas\",\"generation\":\"" + wireGeneration + "\",\"canvas_path\":" + path, true, {});
        };
        auto revisionOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };
        auto recordsOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? JSON::toString(data->getProperty("records"), true) : String();
        };
        auto edit = [&](int id, String const& revision, String const& edits, String const& expectedError) {
            auto result = send(id, "\"operation\":\"edit_canvas\",\"generation\":\"" + wireGeneration + "\",\"expected_revision\":\""
                + revision + "\",\"edits\":" + edits, expectedError.isEmpty(), expectedError);
            if (expectedError == "PartialOrUncertain") {
                auto* envelope = result.getDynamicObject();
                auto* error = envelope ? envelope->getProperty("error").getDynamicObject() : nullptr;
                check(error && error->getProperty("revision").toString().isNotEmpty()
                    && error->getProperty("recovery").isObject(), "dispatched wire edits must return revision-bound recovery, never applied");
            }
            return result;
        };
        auto wire = [](String const& op, String const& path, String const& source, int outlet, String const& target, int inlet) {
            return "{\"op\":\"" + op + "\",\"canvas_path\":" + path + ",\"source\":" + source
                + ",\"outlet\":" + String(outlet) + ",\"target\":" + target + ",\"inlet\":" + String(inlet) + "}";
        };
        auto rootEdge = [&](String const& op, String const& target, int outlet) { return wire(op, "[]", "0", outlet, target, 0); };
        auto childEdge = [&](String const& op, int outlet, int target) { return wire(op, "[3]", "0", outlet, String(target), 0); };
        auto countEdge = [](var const& response, String const& path, int source, int outlet, int target, String const& metadata, int inlet = 0) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            auto* records = data ? data->getProperty("records").getArray() : nullptr;
            int count = 0;
            if (records)
                for (auto const& record : *records) {
                    auto* row = record.getDynamicObject();
                    if (row && row->getProperty("kind").toString() == "connection"
                        && JSON::toString(row->getProperty("canvas_path"), true).removeCharacters(" ") == path
                        && static_cast<int>(row->getProperty("source")) == source
                        && static_cast<int>(row->getProperty("outlet")) == outlet
                        && static_cast<int>(row->getProperty("target")) == target
                        && static_cast<int>(row->getProperty("inlet")) == inlet
                        && (metadata.isEmpty() || row->getProperty("path_info").toString() == metadata))
                        ++count;
                }
            return count;
        };
        send(1300, "\"operation\":\"set_generation\",\"generation\":\"wire-generation\",\"root_receiver\":\"wire-root\"", true, {});
        auto initial = read(1301, "[]");
        String const revision = revisionOf(initial);
        String const initialRoot = recordsOf(initial);
        String const initialChild = recordsOf(read(1302, "[3]"));
        check(countEdge(initial, "[]", 0, 0, 1, "wire-route") == 1, "existing routed wire metadata must be readable");

        auto rejectUnchanged = [&](int id, String const& edits, String const& error) {
            edit(id, revision, edits, error);
            check(revisionOf(read(id + 1, "[]")) == revision && recordsOf(read(id + 2, "[]")) == initialRoot
                    && recordsOf(read(id + 3, "[3]")) == initialChild,
                "invalid wire batch must preserve root and child bytes: " + error);
        };
        rejectUnchanged(1310, "[" + rootEdge("connect", "1", 1) + "," + childEdge("connect", 2, 2) + "]", "InvalidPort");
        rejectUnchanged(1320, "[" + rootEdge("connect", "1", 1) + "," + childEdge("connect", -1, 2) + "]", "InvalidRequest");
        rejectUnchanged(1330, "[" + rootEdge("connect", "1", 1) + "," + wire("connect", "[]", "2", 0, "1", 0) + "]", "InvalidConnection");
        rejectUnchanged(1340, "[" + rootEdge("connect", "1", 0) + "]", "InvalidConnection");
        rejectUnchanged(1350, "[" + rootEdge("disconnect", "1", 1) + "]", "InvalidConnection");
        rejectUnchanged(1360, "[" + rootEdge("connect", "1", 1) + "," + rootEdge("connect", "1", 1) + "]", "InvalidConnection");
        rejectUnchanged(1370, "[" + rootEdge("disconnect", "1", 0) + "," + rootEdge("disconnect", "1", 0) + "]", "InvalidConnection");

        auto result = edit(1380, revision,
            "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"new\",\"type\":\"object\",\"x\":220,\"y\":20,\"text\":\"print new\"},"
            + rootEdge("connect", "{\"ref\":\"new\"}", 1) + "," + childEdge("connect", 1, 2) + ","
            + rootEdge("disconnect", "1", 0) + "," + rootEdge("connect", "1", 0) + "]", "");
        auto* envelope = result.getDynamicObject();
        auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
        auto* maps = data ? data->getProperty("mappings").getDynamicObject() : nullptr;
        auto* refs = maps ? maps->getProperty("refs").getDynamicObject() : nullptr;
        auto* newRef = refs ? refs->getProperty("new").getDynamicObject() : nullptr;
        check(newRef && static_cast<int>(newRef->getProperty("ordinal")) == 4
                && JSON::toString(newRef->getProperty("canvas_path"), true).removeCharacters(" ") == "[]",
            "new endpoint must map to its post-state root ordinal");
        auto rootAfter = read(1381, "[]");
        auto childAfter = read(1382, "[3]");
        check(revisionOf(rootAfter) != revision && data && data->getProperty("revision").toString() == revisionOf(rootAfter),
            "applied wire reply must carry the actual post-state revision");
        check(countEdge(rootAfter, "[]", 0, 0, 1, "wire-route") == 1
                && countEdge(rootAfter, "[]", 0, 1, 4, {}) == 1
                && countEdge(childAfter, "[3]", 0, 0, 1, {}) == 1
                && countEdge(childAfter, "[3]", 0, 1, 2, {}) == 1,
            "root and child cords plus routed metadata must survive ordered edits");
        auto const saved = canvas->patch.getCanvasContent();
        check(saved.contains("#X connect 0 0 1 0 wire-route;") && saved.contains("#X connect 0 1 4 0;")
                && saved.contains("#X connect 0 1 2 0;") && saved.contains("#X obj 220 20 print new;"),
            "native save must serialize both depths and preserved route metadata");

        auto const afterRevision = revisionOf(rootAfter);
        edit(1390, afterRevision, "[" + rootEdge("disconnect", "4", 1) + "," + childEdge("disconnect", 1, 2) + "]", "");
        auto disconnected = read(1391, "[]");
        check(countEdge(disconnected, "[]", 0, 1, 4, {}) == 0
                && countEdge(disconnected, "[3]", 0, 1, 2, {}) == 0
                && countEdge(disconnected, "[]", 0, 0, 1, "wire-route") == 1
                && countEdge(disconnected, "[3]", 0, 0, 1, {}) == 1,
            "targeted disconnect must keep unrelated native cords");
        edit(1392, revisionOf(disconnected), "[" + rootEdge("connect", "4", 1) + "," + childEdge("connect", 1, 2) + "]", "");
        auto reconnected = read(1393, "[]");
        check(countEdge(reconnected, "[]", 0, 1, 4, {}) == 1
                && countEdge(reconnected, "[3]", 0, 1, 2, {}) == 1,
            "targeted reconnect must restore only intended cords");
        editor->pd->lockAudioThread();
        auto* nativeRoot = canvas->patch.getRawPointer();
        auto* nativeChild = nativeRoot->gl_list;
        auto* nativeAdded = nativeRoot->gl_list;
        for (int ordinal = 0; nativeChild && ordinal < 3; ++ordinal)
            nativeChild = nativeChild->g_next;
        for (int ordinal = 0; nativeAdded && ordinal < 4; ++ordinal)
            nativeAdded = nativeAdded->g_next;
        bool const nativeEdges = nativeAdded && nativeChild && pd_class(&nativeChild->g_pd) == canvas_class
            && pd::Interface::hasConnection(nativeRoot, pd::Interface::checkObject(nativeRoot->gl_list), 1,
                pd::Interface::checkObject(nativeAdded), 0)
            && pd::Interface::hasConnection(reinterpret_cast<t_canvas*>(nativeChild),
                pd::Interface::checkObject(reinterpret_cast<t_canvas*>(nativeChild)->gl_list), 1,
                pd::Interface::checkObject(reinterpret_cast<t_canvas*>(nativeChild)->gl_list->g_next->g_next), 0);
        editor->pd->unlockAudioThread();
        check(nativeEdges, "native root and child graph must contain the reconnected cords");
        auto lateInvalid = edit(1395, revisionOf(reconnected),
            "[{\"op\":\"add\",\"canvas_path\":[3],\"ref\":\"late\",\"type\":\"object\",\"x\":180,\"y\":90,\"text\":\"print late\"},"
            + wire("connect", "[3]", "{\"ref\":\"late\"}", 1, "2", 0) + "]", "PartialOrUncertain");
        auto* lateEnvelope = lateInvalid.getDynamicObject();
        auto* lateError = lateEnvelope ? lateEnvelope->getProperty("error").getDynamicObject() : nullptr;
        auto lateReadback = read(1396, "[3]");
        check(lateError && lateError->getProperty("revision").toString() == revisionOf(lateReadback)
                && countEdge(lateReadback, "[3]", 0, 0, 1, {}) == 1
                && countEdge(lateReadback, "[3]", 0, 1, 2, {}) == 1
                && recordsOf(lateReadback).contains("print late"),
            "newly created endpoint must have native ports rechecked and report uncertain partial add, never applied");
        closeExportFixture(canvas, "wire-root");
        String const portFixture = "#N canvas 100 100 400 300 12;\n"
            "#X obj 10 10 t b b;\n#X obj 100 10 print first;\n#X obj 200 10 print second;\n"
            "#X obj 300 10 print unrelated;\n"
            "#N canvas 0 0 240 180 ports 0;\n"
            "#X obj 20 20 inlet;\n#X obj 100 20 inlet;\n"
            "#X obj 20 80 outlet;\n#X obj 100 80 outlet;\n"
            "#X connect 0 0 2 0;\n#X connect 1 0 3 0;\n"
            "#X restore 20 90 pd ports;\n"
            "#X connect 0 0 4 0;\n#X connect 0 1 4 1;\n"
            "#X connect 4 0 1 0;\n#X connect 4 1 2 0;\n"
            "#X connect 0 0 3 0;\n";
        auto* portCanvas = openBoundExportFixture(portFixture, "wire-ports-root");
        check(portCanvas != nullptr, "nested port fixture must open");
        if (portCanvas) {
            wireGeneration = "wire-ports-generation";
            send(1400, "\"operation\":\"set_generation\",\"generation\":\"wire-ports-generation\",\"root_receiver\":\"wire-ports-root\"", true, {});
            auto portInitial = read(1401, "[]");
            auto const portRevision = revisionOf(portInitial);
            auto const portRootBytes = recordsOf(portInitial);
            auto const portChildBytes = recordsOf(read(1402, "[4]"));
            check(countEdge(portInitial, "[]", 4, 0, 1, {}) == 1
                    && countEdge(portInitial, "[]", 4, 1, 2, {}) == 1
                    && countEdge(portInitial, "[]", 0, 0, 3, {}) == 1,
                "two child outlets and an unrelated parent cord must exist before removal");
            edit(1403, portRevision,
                "[{\"op\":\"remove\",\"canvas_path\":[4],\"target\":2},"
                + wire("disconnect", "[]", "4", 0, "1", 0) + "]", "InvalidConnection");
            auto portRootAfterRejection = read(1404, "[]");
            auto portChildAfterRejection = read(1405, "[4]");
            check(revisionOf(portRootAfterRejection) == portRevision
                    && recordsOf(portRootAfterRejection) == portRootBytes
                    && recordsOf(portChildAfterRejection) == portChildBytes,
                "removing child outlet then disconnecting its parent edge must reject before any Pd mutation");
            auto rejectPortChange = [&](int id, int childOrdinal, String const& later, String const& code) {
                edit(id, portRevision, "[{\"op\":\"remove\",\"canvas_path\":[4],\"target\":"
                    + String(childOrdinal) + "}," + later + "]", code);
                auto rootReadback = read(id + 1, "[]");
                auto childReadback = read(id + 2, "[4]");
                check(revisionOf(rootReadback) == portRevision && recordsOf(rootReadback) == portRootBytes
                        && recordsOf(childReadback) == portChildBytes,
                    "invalid shifted parent port must leave root and child snapshots unchanged");
            };
            rejectPortChange(1406, 0, wire("disconnect", "[]", "0", 0, "4", 0), "InvalidConnection");
            rejectPortChange(1409, 2, wire("connect", "[]", "4", 1, "1", 0), "InvalidPort");
            rejectPortChange(1412, 0, wire("connect", "[]", "0", 0, "4", 1), "InvalidPort");
            edit(1415, portRevision,
                "[{\"op\":\"remove\",\"canvas_path\":[4],\"target\":2},"
                + wire("disconnect", "[]", "4", 0, "2", 0) + "]", "");
            auto outletAfter = read(1416, "[]");
            auto outletRevision = revisionOf(outletAfter);
            check(outletRevision != portRevision && countEdge(outletAfter, "[]", 4, 0, 1, {}) == 0
                    && countEdge(outletAfter, "[]", 4, 0, 2, {}) == 0
                    && countEdge(outletAfter, "[]", 0, 0, 3, {}) == 1
                    && countEdge(outletAfter, "[]", 0, 0, 4, {}) == 1
                    && countEdge(outletAfter, "[]", 0, 1, 4, {}, 1) == 1,
                "valid shifted outlet disconnect must remove only the surviving shifted parent edge");
            edit(1417, outletRevision,
                "[{\"op\":\"remove\",\"canvas_path\":[4],\"target\":0},"
                + wire("disconnect", "[]", "0", 1, "4", 0) + "]", "");
            auto inletAfter = read(1418, "[]");
            check(countEdge(inletAfter, "[]", 0, 0, 4, {}) == 0
                    && countEdge(inletAfter, "[]", 0, 1, 4, {}) == 0
                    && countEdge(inletAfter, "[]", 0, 0, 3, {}) == 1,
                "valid shifted inlet disconnect must preserve unrelated root wire");
            editor->pd->lockAudioThread();
            auto* parent = portCanvas->patch.getRawPointer()->gl_list;
            for (int ordinal = 0; parent && ordinal < 4; ++ordinal)
                parent = parent->g_next;
            bool const nativePorts = parent && pd_class(&parent->g_pd) == canvas_class
                && pd::Interface::numOutlets(pd::Interface::checkObject(parent)) == 1
                && pd::Interface::numInlets(pd::Interface::checkObject(parent)) == 1;
            editor->pd->unlockAudioThread();
            check(nativePorts && portCanvas->patch.getCanvasContent().contains("#X connect 0 0 3 0;")
                    && !portCanvas->patch.getCanvasContent().contains("#X connect 4 0 2 0;"),
                "native port counts and saved graph must agree with projected port removal");
            closeExportFixture(portCanvas, "wire-ports-root");
        }
        String const tiedFixture = "#N canvas 100 100 400 300 12;\n"
            "#X obj 10 10 t b b;\n#X obj 100 10 print first;\n#X obj 200 10 print second;\n"
            "#X obj 300 10 print unrelated;\n"
            "#N canvas 0 0 240 180 tied 0;\n"
            "#X obj 40 20 inlet;\n#X obj 40 80 inlet;\n"
            "#X obj 20 20 outlet;\n#X obj 20 80 outlet;\n"
            "#X connect 0 0 2 0;\n#X connect 1 0 3 0;\n"
            "#X restore 20 90 pd tied;\n"
            "#X connect 0 0 4 0;\n#X connect 0 1 4 1;\n"
            "#X connect 4 0 1 0;\n#X connect 4 1 2 0;\n"
            "#X connect 0 0 3 0;\n";
        for (bool const inletPort : { false, true }) {
            int const id = inletPort ? 1430 : 1420;
            String const receiver = inletPort ? "wire-tied-inlet-root" : "wire-tied-outlet-root";
            wireGeneration = inletPort ? "wire-tied-inlet-generation" : "wire-tied-outlet-generation";
            auto* tiedCanvas = openBoundExportFixture(tiedFixture, receiver);
            check(tiedCanvas != nullptr, "equal-x parent port fixture must open");
            if (!tiedCanvas)
                continue;
            send(id, "\"operation\":\"set_generation\",\"generation\":\"" + wireGeneration
                + "\",\"root_receiver\":\"" + receiver + "\"", true, {});
            auto tiedRoot = read(id + 1, "[]");
            auto const tiedRevision = revisionOf(tiedRoot);
            auto const tiedRootBytes = recordsOf(tiedRoot);
            auto const tiedChildBytes = recordsOf(read(id + 2, "[4]"));
            String const removePort = "{\"op\":\"remove\",\"canvas_path\":[4],\"target\":"
                + String(inletPort ? 0 : 2) + "}";
            String const deletedEdge = inletPort ? wire("disconnect", "[]", "0", 1, "4", 0)
                                                 : wire("disconnect", "[]", "4", 0, "2", 0);
            edit(id + 3, tiedRevision, "[" + removePort + "," + deletedEdge + "]", "InvalidConnection");
            auto tiedRootAfterRejection = read(id + 4, "[]");
            auto tiedChildAfterRejection = read(id + 5, "[4]");
            bool const tiedUnchanged = revisionOf(tiedRootAfterRejection) == tiedRevision
                && recordsOf(tiedRootAfterRejection) == tiedRootBytes
                && recordsOf(tiedChildAfterRejection) == tiedChildBytes;
            check(tiedUnchanged, "equal-x deleted parent port must reject before any root/child mutation");
            if (tiedUnchanged) {
                String const survivingEdge = inletPort ? wire("disconnect", "[]", "0", 0, "4", 0)
                                                         : wire("disconnect", "[]", "4", 0, "1", 0);
                edit(id + 6, tiedRevision, "[" + removePort + "," + survivingEdge + "]", "");
                auto tiedAfter = read(id + 7, "[]");
                check(revisionOf(tiedAfter) != tiedRevision && countEdge(tiedAfter, "[]", 0, 0, 3, {}) == 1
                        && (inletPort ? countEdge(tiedAfter, "[]", 0, 0, 4, {}) == 0
                                && countEdge(tiedAfter, "[]", 0, 1, 4, {}, 1) == 0
                            : countEdge(tiedAfter, "[]", 4, 0, 1, {}) == 0
                                && countEdge(tiedAfter, "[]", 4, 1, 2, {}) == 0),
                    "equal-x surviving parent cord must disconnect at its shifted native index, leaving unrelated wire");
                auto savedTied = tiedCanvas->patch.getCanvasContent();
                check(savedTied.contains("#X connect 0 0 3 0;")
                        && (inletPort ? !savedTied.contains("#X connect 0 0 4 0;")
                                      : !savedTied.contains("#X connect 4 0 1 0;")),
                    "saved Pd source must agree with equal-x native cord deletion");
                editor->pd->lockAudioThread();
                auto* root = tiedCanvas->patch.getRawPointer();
                auto* first = root->gl_list;
                auto* second = first ? first->g_next : nullptr;
                auto* unrelated = second && second->g_next ? second->g_next->g_next : nullptr;
                auto* nativeParent = unrelated ? unrelated->g_next : nullptr;
                bool const nativeGraph = first && second && unrelated && nativeParent
                    && pd_class(&nativeParent->g_pd) == canvas_class
                    && pd::Interface::hasConnection(root, pd::Interface::checkObject(first), 0,
                        pd::Interface::checkObject(unrelated), 0)
                    && (inletPort
                        ? pd::Interface::numInlets(pd::Interface::checkObject(nativeParent)) == 1
                            && pd::Interface::hasConnection(root, pd::Interface::checkObject(nativeParent), 0,
                                pd::Interface::checkObject(second), 0)
                        : pd::Interface::numOutlets(pd::Interface::checkObject(nativeParent)) == 1
                            && pd::Interface::hasConnection(root, pd::Interface::checkObject(first), 0,
                                pd::Interface::checkObject(nativeParent), 0));
                editor->pd->unlockAudioThread();
                check(nativeGraph, "equal-x native graph must retain other-direction and unrelated parent cords");
            }
            closeExportFixture(tiedCanvas, receiver);
        }
        send(1394, "\"operation\":\"set_generation\",\"generation\":\"generation-1\",\"root_receiver\":\"" + rootReceiver + "\"", true, {});
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-wire] fixture closed" << std::endl;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasPortMoveRouting()
    {
        beginTest("Child port move routing");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        auto send = [this](int id, String const& body, bool ok, String const& error) {
            var request;
            String const json = "{\"version\":1,\"request_id\":" + String(id) + "," + body + "}";
            check(JSON::parse(json, request).wasOk(), "port routing request must parse");
            consoleBegin();
            sendRequest(request, id, ok, error);
            flushDebugQueue();
            check(responses.size() == 1, "port routing request has one reply");
            checkConsoleReply(0, id, ok, error);
            var const reply = responses.isEmpty() ? var() : responses[0];
            std::cout << "[canvas-port-move] request=" << json << " reply=" << JSON::toString(reply, true) << std::endl;
            return reply;
        };
        auto revisionOf = [](var const& reply) {
            auto* envelope = reply.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };
        auto route = [&](int id, String const& generation, String const& label, bool signal) {
            int const start = printLines.size();
            for (int source = 0; source < (signal ? 1 : 2); ++source)
                send(id + source, "\"operation\":\"send_object\",\"generation\":\"" + generation
                    + "\",\"canvas_path\":[],\"object_ordinal\":" + String(source)
                    + ",\"selector\":\"bang\",\"atoms\":[]", true, {});
            bool first = false, second = false, crossed = false;
            for (int index = start; index < printLines.size(); ++index) {
                first |= printLines[index].contains("port-A: 11");
                second |= printLines[index].contains("port-B: 22");
                crossed |= printLines[index].contains("port-A: 22") || printLines[index].contains("port-B: 11");
            }
            std::cout << "[canvas-port-move] " << label << " left=" << first << " right=" << second << " crossed=" << crossed << std::endl;
            check(first && (signal || second) && !crossed, label + " must keep parent routes attached to their original child port objects");
        };
        int id = 1600;
        for (int variant = 0; variant < 6; ++variant) {
            bool const inlet = variant % 2 == 0;
            bool const signal = variant == 2 || variant == 3;
            bool const tied = variant >= 4;
            String const label = String(inlet ? "inlet" : "outlet") + (signal ? "-signal" : tied ? "-tied" : "");
            String const receiver = "port-move-" + label;
            String const generation = "port-move-generation-" + label;
            String const fixture = "#N canvas 100 100 400 300 12;\n"
                "#X obj 10 10 f 11;\n#X obj 100 10 " + String(signal ? "sig~ 220" : "f 22") + ";\n"
                "#N canvas 0 0 240 180 ports 0;\n"
                "#X obj 20 20 inlet;\n#X obj 100 20 " + String(signal ? "inlet~" : "inlet") + ";\n"
                "#X obj 20 80 outlet;\n#X obj 100 80 " + String(signal ? "outlet~" : "outlet") + ";\n"
                "#X connect 0 0 2 0;\n#X connect 1 0 3 0;\n"
                "#X restore 20 90 pd ports;\n"
                "#X obj 200 10 print port-A;\n#X obj 300 10 " + String(signal ? "snapshot~" : "print port-B") + ";\n"
                "#X connect 0 0 2 0;\n#X connect 1 0 2 1;\n"
                "#X connect 2 0 3 0;\n#X connect 2 1 4 0;\n";
            auto* canvas = openBoundExportFixture(fixture, receiver);
            check(canvas != nullptr, "port routing fixture must open");
            if (!canvas)
                continue;
            send(id++, "\"operation\":\"set_generation\",\"generation\":\"" + generation
                + "\",\"root_receiver\":\"" + receiver + "\"", true, {});
            auto read = [&] { return send(id++, "\"operation\":\"read_canvas\",\"generation\":\""
                + generation + "\",\"canvas_path\":[]", true, {}); };
            String const revision = revisionOf(read());
            route(id, generation, label + " before", signal);
            id += 2;
            String const move = "{\"op\":\"move\",\"canvas_path\":[2],\"target\":"
                + String(inlet ? 0 : 2) + ",\"x\":" + String(tied ? 100 : 140)
                + ",\"y\":" + String(inlet ? 20 : 80) + "}";
            String const staleWire = inlet
                ? "{\"op\":\"connect\",\"canvas_path\":[],\"source\":0,\"outlet\":0,\"target\":2,\"inlet\":1}"
                : "{\"op\":\"disconnect\",\"canvas_path\":[],\"source\":2,\"outlet\":0,\"target\":3,\"inlet\":0}";
            auto const before = canvas->patch.getCanvasContent();
            send(id++, "\"operation\":\"edit_canvas\",\"generation\":\"" + generation
                + "\",\"expected_revision\":\"" + revision + "\",\"edits\":[" + move + "," + staleWire + "]",
                false, "InvalidConnection");
            check(revisionOf(read()) == revision && canvas->patch.getCanvasContent() == before,
                "later invalid parent wire must reject before moving child " + label);
            auto moved = send(id++, "\"operation\":\"edit_canvas\",\"generation\":\"" + generation
                + "\",\"expected_revision\":\"" + revision + "\",\"edits\":[" + move + "]", true, {});
            auto* data = moved.getDynamicObject() ? moved.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
            check(data && data->getProperty("status").toString() == "applied" && revisionOf(read()) == revisionOf(moved)
                    && revisionOf(moved) != revision, "port move acknowledges its changed native revision");
            route(id, generation, label + " live", signal);
            id += 2;
            if (signal) {
                editor->pd->lockAudioThread();
                auto* native = canvas->patch.getRawPointer();
                auto* parent = native->gl_list->g_next->g_next;
                auto* nativeObject = pd::Interface::checkObject(parent);
                bool const typesMatch = inlet
                    ? pd::Interface::isSignalInlet(nativeObject, 0) && !pd::Interface::isSignalInlet(nativeObject, 1)
                    : pd::Interface::isSignalOutlet(nativeObject, 0) && !pd::Interface::isSignalOutlet(nativeObject, 1);
                editor->pd->unlockAudioThread();
                check(typesMatch, label + " live parent signal/control index order must follow child x order");
            }
            auto const saved = canvas->patch.getCanvasContent();
            bool const reordered = inlet
                ? saved.contains("#X connect 0 0 2 1;") && saved.contains("#X connect 1 0 2 0;")
                : saved.contains("#X connect 2 1 3 0;") && saved.contains("#X connect 2 0 4 0;");
            std::cout << "[canvas-port-move] " << label << " serialized=" << saved << std::endl;
            check(reordered, "native save must encode rerouted parent " + label + " indices");
            closeExportFixture(canvas, receiver);
            auto* reopened = openBoundExportFixture(saved, receiver);
            check(reopened != nullptr, "serialized port fixture must reopen");
            if (reopened) {
                send(id++, "\"operation\":\"set_generation\",\"generation\":\"" + generation
                    + "\",\"root_receiver\":\"" + receiver + "\"", true, {});
                route(id, generation, label + " reopened", signal);
                id += 2;
                check(reopened->patch.getCanvasContent() == saved,
                    "saved and reopened " + label + " structure must match");
                if (signal) {
                    editor->pd->lockAudioThread();
                    auto* native = reopened->patch.getRawPointer();
                    auto* parent = native->gl_list->g_next->g_next;
                    auto* nativeObject = pd::Interface::checkObject(parent);
                    bool const typesMatch = inlet
                        ? pd::Interface::isSignalInlet(nativeObject, 0) && !pd::Interface::isSignalInlet(nativeObject, 1)
                        : pd::Interface::isSignalOutlet(nativeObject, 0) && !pd::Interface::isSignalOutlet(nativeObject, 1);
                    editor->pd->unlockAudioThread();
                    check(typesMatch, label + " reopened parent signal/control index order must equal live order");
                }
                closeExportFixture(reopened, receiver);
            }
        }
        registerExportGeneration(id++, "generation-1", rootReceiver);
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-port-move] fixture closed" << std::endl;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasAddKindsAndRefs()
    {
        beginTest("Canvas add kinds and refs");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const fixture = "#N canvas 100 100 400 300 12;\n#X obj 10 10 print root;\n"
            "#N canvas 0 0 200 100 twin 0;\n#X obj 10 10 print first;\n#X restore 20 60 pd twin;\n"
            "#N canvas 0 0 200 100 twin 0;\n#X obj 10 10 print second;\n#X restore 140 60 pd twin;\n";
        auto* canvas = openBoundExportFixture(fixture, "add-root");
        check(canvas != nullptr, "add fixture must open");
        if (!canvas) {
            expectedReplies = previousReplies;
            responses = previousResponses;
            beginTest("Versioned receiver and generation registration");
            return;
        }

        auto send = [this](int const id, String const& body, bool const ok, String const& error) {
            String const json = "{\"version\":1,\"request_id\":" + String(id) + "," + body + "}";
            var request;
            check(JSON::parse(json, request).wasOk(), "add request must parse");
            consoleBegin();
            sendRequest(request, id, ok, error);
            flushDebugQueue();
            check(responses.size() == 1, "add request must return one correlated reply");
            checkConsoleReply(0, id, ok, error);
            var result = responses.isEmpty() ? var() : responses[0];
            std::cout << "[canvas-add] request=" << json << " reply=" << JSON::toString(result, true) << std::endl;
            return result;
        };
        auto read = [&](int id, String const& path) {
            return send(id, "\"operation\":\"read_canvas\",\"generation\":\"add-generation\",\"canvas_path\":" + path, true, {});
        };
        auto revisionOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };
        auto recordsOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("records").getArray() : nullptr;
        };
        auto edit = [&](int id, String const& revision, String const& edits, String const& error) {
            return send(id, "\"operation\":\"edit_canvas\",\"generation\":\"add-generation\",\"expected_revision\":\"" + revision
                + "\",\"edits\":" + edits, error.isEmpty(), error);
        };

        send(900, "\"operation\":\"set_generation\",\"generation\":\"add-generation\",\"root_receiver\":\"add-root\"", true, {});
        auto initial = read(901, "[]");
        auto const initialRevision = revisionOf(initial);
        auto* initialRecords = recordsOf(initial);
        check(initialRecords && initialRecords->size() == 8, "initial root has three objects and two child canvases with one object each");
        String const rejected = "[{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"later\"}],\"ref\":\"bad\",\"type\":\"comment\",\"x\":1,\"y\":2,\"text\":\"bad\"},"
            "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"later\",\"type\":\"subpatch\",\"x\":1,\"y\":2,\"name\":\"later\"}]";
        edit(902, initialRevision, rejected, "InvalidRequest");
        edit(903, initialRevision, "[{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"missing\"}],\"ref\":\"bad\",\"type\":\"object\",\"x\":1,\"y\":2,\"text\":\"print bad\"}]", "InvalidRequest");
        edit(904, initialRevision, "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"gone\",\"type\":\"subpatch\",\"x\":1,\"y\":2,\"name\":\"gone\"},"
            "{\"op\":\"remove\",\"canvas_path\":[],\"target\":{\"ref\":\"gone\"}},"
            "{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"gone\"}],\"ref\":\"bad\",\"type\":\"comment\",\"x\":1,\"y\":2,\"text\":\"bad\"}]", "InvalidRequest");
        edit(920, initialRevision, "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"hidden-child\",\"type\":\"object\",\"x\":1,\"y\":2,\"text\":\"pd unintended\"}]", "InvalidRequest");
        check(revisionOf(read(905, "[]")) == initialRevision, "rejected refs must leave root and children unchanged");

        auto result = edit(906, initialRevision,
            "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"root-box\",\"type\":\"object\",\"x\":240,\"y\":20,\"text\":\"print added\"},"
            "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"root-note\",\"type\":\"comment\",\"x\":240,\"y\":50,\"text\":\"root note\"},"
            "{\"op\":\"add\",\"canvas_path\":[1],\"ref\":\"first-note\",\"type\":\"comment\",\"x\":30,\"y\":40,\"text\":\"first note\"},"
            "{\"op\":\"add\",\"canvas_path\":[2],\"ref\":\"second-box\",\"type\":\"object\",\"x\":30,\"y\":40,\"text\":\"print second-added\"},"
            "{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"new-child\",\"type\":\"subpatch\",\"x\":240,\"y\":100,\"name\":\"twin\"},"
            "{\"op\":\"add\",\"canvas_path\":[{\"ref\":\"new-child\"}],\"ref\":\"inner-box\",\"type\":\"object\",\"x\":20,\"y\":20,\"text\":\"print nested\"},"
            "{\"op\":\"move\",\"canvas_path\":[{\"ref\":\"new-child\"}],\"target\":{\"ref\":\"inner-box\"},\"x\":50,\"y\":60},"
            "{\"op\":\"add\",\"canvas_path\":[1],\"ref\":\"deep-child\",\"type\":\"subpatch\",\"x\":70,\"y\":100,\"name\":\"deep\"}]", "");
        auto* envelope = result.getDynamicObject();
        auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
        auto* maps = data ? data->getProperty("mappings").getDynamicObject() : nullptr;
        auto* refs = maps ? maps->getProperty("refs").getDynamicObject() : nullptr;
        auto* childRef = refs ? refs->getProperty("new-child").getDynamicObject() : nullptr;
        auto* childPath = childRef ? childRef->getProperty("canvas_path").getArray() : nullptr;
        check(childPath && childPath->size() == 1 && static_cast<int>((*childPath)[0]) == 5,
            "new child ref must map to its post-state ordinal path without using its name");
        auto* deepRef = refs ? refs->getProperty("deep-child").getDynamicObject() : nullptr;
        auto* deepPath = deepRef ? deepRef->getProperty("canvas_path").getArray() : nullptr;
        check(deepPath && deepPath->size() == 2 && static_cast<int>((*deepPath)[0]) == 1
            && static_cast<int>((*deepPath)[1]) == 2, "nested empty subpatch must map to its post-state path");
        auto rootAfter = read(907, "[]");
        auto firstAfter = read(908, "[1]");
        auto secondAfter = read(909, "[2]");
        auto newAfter = read(910, "[5]");
        auto emptyAfter = read(919, "[1,2]");
        auto* newRecords = recordsOf(newAfter);
        check(revisionOf(rootAfter) != initialRevision && data && data->getProperty("revision").toString() == revisionOf(rootAfter),
            "applied reply must bind the actual post-state readback revision");
        check(newRecords && newRecords->size() == 2 && (*newRecords)[1].getDynamicObject()
            && static_cast<int>((*newRecords)[1].getDynamicObject()->getProperty("x")) == 50
            && static_cast<int>((*newRecords)[1].getDynamicObject()->getProperty("y")) == 60,
            "new child must contain the moved live object");
        check(recordsOf(emptyAfter) && recordsOf(emptyAfter)->size() == 1,
            "new nested subpatch must be empty and reachable by its post-state path");
        check(recordsOf(firstAfter) && recordsOf(firstAfter)->size() == 5
            && recordsOf(secondAfter) && recordsOf(secondAfter)->size() == 3,
            "equal-named pre-existing siblings must each receive only their addressed addition");
        editor->pd->lockAudioThread();
        auto* root = canvas->patch.getRawPointer();
        int rootCount = 0;
        for (auto* item = root->gl_list; item; item = item->g_next)
            ++rootCount;
        auto* child = root->gl_list;
        for (int index = 0; child && index < 5; ++index)
            child = child->g_next;
        bool const nativeChild = child && pd_class(&child->g_pd) == canvas_class
            && reinterpret_cast<t_canvas*>(child)->gl_owner == root;
        bool const nativeObject = nativeChild && reinterpret_cast<t_canvas*>(child)->gl_list
            && !reinterpret_cast<t_canvas*>(child)->gl_list->g_next;
        editor->pd->unlockAudioThread();
        check(rootCount == 6 && nativeChild && nativeObject, "live root list must grow and native empty subpatch must own its later child object");
        check(canvas->patch.getCanvasContent().contains("#X restore 240 100 pd twin;")
            && canvas->patch.getCanvasContent().contains("#X obj 50 60 print nested;")
            && canvas->patch.getCanvasContent().contains("#X restore 70 100 pd deep;"),
            "native subpatch must serialize its restore entry and nested object, not a graph");

        edit(911, initialRevision, "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"stale\",\"type\":\"comment\",\"x\":1,\"y\":2,\"text\":\"bad\"}]", "StaleRevision");
        auto const postRevision = revisionOf(rootAfter);
        send(915, "\"operation\":\"edit_canvas\",\"generation\":\"wrong-generation\",\"expected_revision\":\"" + postRevision
            + "\",\"edits\":[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"stale-generation\",\"type\":\"object\",\"x\":1,\"y\":2,\"text\":\"print no\"}]", false, "StaleGeneration");
        check(revisionOf(read(916, "[]")) == postRevision, "stale generation and revision must not dispatch additions");
        edit(912, postRevision, "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"partial\",\"type\":\"comment\",\"x\":10,\"y\":130,\"text\":\"partial\"},"
            "{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":30,\"y\":30}]", "");
        auto const partialRevision = revisionOf(read(913, "[]"));
        check(partialRevision != postRevision, "unsupported follow-up must expose the partial edit through readback");
        edit(917, partialRevision, "[{\"op\":\"add\",\"canvas_path\":[],\"ref\":\"unknown\",\"type\":\"object\",\"x\":10,\"y\":160,\"text\":\"unknown_canvas_add_class_917\"}]", "PartialOrUncertain");
        check(revisionOf(read(918, "[]")) != partialRevision, "unknown object class must be visible as a failed creation stub in recovery, never acknowledged as applied");
        closeExportFixture(canvas, "add-root");
        send(914, "\"operation\":\"set_generation\",\"generation\":\"generation-1\",\"root_receiver\":\"" + rootReceiver + "\"", true, {});
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-add] root and child fixture closed" << std::endl;
        beginTest("Versioned receiver and generation registration");
    }

    void testCanvasChildMoveDeleteAndRootSurvivor()
    {
        beginTest("Child move delete and root survivor");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const fixture = "#N canvas 100 100 400 300 12;\n"
            "#X obj 10 10 f 0;\n#X obj 100 10 print root-state;\n#X obj 10 50 t b b;\n#X obj 100 50 print root-remove;\n"
            "#N canvas 0 0 220 160 twin 0;\n#X obj 10 10 f 0;\n#X obj 90 10 print first-state;\n"
            "#X obj 10 50 t b b;\n#X obj 90 50 print first-remove;\n#X connect 0 0 1 0;\n#X connect 2 0 3 0;\n#X restore 180 100 pd twin;\n"
            "#N canvas 0 0 220 160 twin 0;\n#X obj 10 10 f 0;\n#X obj 90 10 print second-state;\n"
            "#X connect 0 0 1 0;\n#X restore 280 100 pd twin;\n"
            "#X connect 0 0 1 0;\n#X connect 2 0 3 0;\n";
        auto* canvas = openBoundExportFixture(fixture, "move-delete-root");
        check(canvas != nullptr, "move/delete fixture must open");
        if (!canvas) {
            expectedReplies = previousReplies;
            responses = previousResponses;
            beginTest("Versioned receiver and generation registration");
            return;
        }

        auto send = [this](int const id, String const& json, bool const ok, String const& error, String const& status) {
            var request;
            check(JSON::parse(json, request).wasOk(), "move/delete receiver request must parse");
            consoleBegin();
            sendRequest(request, id, ok, error);
            if (status.isNotEmpty())
                expectedReplies.back().successStatus = status;
            flushDebugQueue();
            check(responses.size() == 1, "move/delete request must return one correlated reply");
            checkConsoleReply(0, id, ok, error);
            auto result = responses.isEmpty() ? var() : responses[0];
            std::cout << "[canvas-move-delete] request=" << json << " reply=" << JSON::toString(result, true) << std::endl;
            return result;
        };
        auto read = [&](int const id, String const& path) {
            return send(id, "{\"version\":1,\"request_id\":" + String(id)
                + ",\"operation\":\"read_canvas\",\"generation\":\"move-delete-generation\",\"canvas_path\":" + path + "}", true, {}, {});
        };
        auto revisionOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };
        auto recordsOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("records").getArray() : nullptr;
        };
        auto edit = [&](int const id, String const& revision, String const& edits, String const& expectedError) {
            return send(id, "{\"version\":1,\"request_id\":" + String(id)
                + ",\"operation\":\"edit_canvas\",\"generation\":\"move-delete-generation\",\"expected_revision\":\""
                + revision + "\",\"edits\":" + edits + "}", expectedError.isEmpty(), expectedError, {});
        };
        auto sendObject = [&](int const id, Array<int> const& path, int const ordinal, String const& selector, Array<var> const& atoms) {
            auto const request = makeSendObjectRequest(id, "move-delete-generation", path, ordinal, selector, atoms);
            return send(id, JSON::toString(request, true), true, {}, "invoked");
        };
        auto sameAddress = [](var const& value, std::initializer_list<int> expected) {
            auto const* address = value.getArray();
            if (!address || address->size() != static_cast<int>(expected.size()))
                return false;
            int index = 0;
            for (auto const ordinal : expected)
                if (static_cast<int>((*address)[index++]) != ordinal)
                    return false;
            return true;
        };
        auto hasSurvivor = [&](Array<var> const* mappings, std::initializer_list<int> before, std::initializer_list<int> after) {
            if (!mappings)
                return false;
            for (auto const& value : *mappings) {
                auto* entry = value.getDynamicObject();
                if (entry && sameAddress(entry->getProperty("before"), before) && sameAddress(entry->getProperty("after"), after))
                    return true;
            }
            return false;
        };
        auto hasInvalidated = [&](Array<var> const* mappings, std::initializer_list<int> address) {
            if (!mappings)
                return false;
            for (auto const& value : *mappings) {
                if (sameAddress(value, address))
                    return true;
                auto* entry = value.getDynamicObject();
                if (!entry || !static_cast<bool>(entry->getProperty("descendants")))
                    continue;
                if (sameAddress(entry->getProperty("before"), address))
                    return true;
                auto const* prefix = entry->getProperty("before").getArray();
                if (!prefix || prefix->size() >= static_cast<int>(address.size()))
                    continue;
                int index = 0;
                bool matches = true;
                for (auto const ordinal : address) {
                    if (index == prefix->size())
                        break;
                    matches = matches && static_cast<int>((*prefix)[index++]) == ordinal;
                }
                if (matches)
                    return true;
            }
            return false;
        };
        auto hasConnection = [](Array<var> const* records, int const source, int const target) {
            if (!records)
                return false;
            for (auto const& value : *records) {
                auto* record = value.getDynamicObject();
                if (record && record->getProperty("kind").toString() == "connection"
                    && static_cast<int>(record->getProperty("source")) == source
                    && static_cast<int>(record->getProperty("target")) == target)
                    return true;
            }
            return false;
        };
        auto sameRecords = [](Array<var> const* left, Array<var> const* right) {
            return left && right && JSON::toString(var(*left), true) == JSON::toString(var(*right), true);
        };

        send(1000, "{\"version\":1,\"request_id\":1000,\"operation\":\"set_generation\",\"generation\":\"move-delete-generation\",\"root_receiver\":\"move-delete-root\"}", true, {}, {});
        auto initial = read(1001, "[]");
        String const initialRevision = revisionOf(initial);
        auto* initialRecords = recordsOf(initial);
        check(initialRevision.length() == 64 && initialRecords && initialRecords->size() == 20,
            "same-name fixture must expose the root and both complete child canvases");
        editor->pd->lockAudioThread();
        auto* rootBeforeEdit = canvas->patch.getRawPointer();
        canvas_dirty(rootBeforeEdit, 0);
        bool const rootWasClean = rootBeforeEdit->gl_dirty == 0;
        editor->pd->unlockAudioThread();
        check(rootWasClean, "native fixture clean marker must be clear before the edit");

        sendObject(1002, {}, 0, "float", { 42 });
        sendObject(1003, { 4 }, 0, "float", { 77 });
        sendObject(1004, { 5 }, 0, "float", { 88 });
        check(printLines.contains("root-state: 42") && printLines.contains("first-state: 77")
                && printLines.contains("second-state: 88"),
            "root and both equal-named child stateful f objects must receive their assigned values");
        printLines.clear();
        printBuffer.clear();

        String const deletedDescendant = "[{\"op\":\"remove\",\"canvas_path\":[],\"target\":4},"
            "{\"op\":\"move\",\"canvas_path\":[4],\"target\":0,\"x\":15,\"y\":25}]";
        edit(1005, initialRevision, deletedDescendant, "CanvasNotFound");
        auto unchanged = read(1006, "[]");
        check(revisionOf(unchanged) == initialRevision && sameRecords(recordsOf(unchanged), initialRecords),
            "a mixed batch that targets a deleted descendant must reject before any move/remove mutation");

        editor->pd->lockAudioThread();
        int const initialDsp = canvas_dspstate;
        editor->pd->unlockAudioThread();
        editor->pd->startDSP();
        editor->pd->lockAudioThread();
        bool const dspEnabledBeforeEdit = canvas_dspstate != 0;
        editor->pd->unlockAudioThread();
        check(dspEnabledBeforeEdit, "DSP must be enabled before scoped deletion");

        String const operations = "[{\"op\":\"move\",\"canvas_path\":[],\"target\":0,\"x\":35,\"y\":35},"
            "{\"op\":\"remove\",\"canvas_path\":[],\"target\":2},"
            "{\"op\":\"move\",\"canvas_path\":[4],\"target\":1,\"x\":70,\"y\":35},"
            "{\"op\":\"remove\",\"canvas_path\":[4],\"target\":2}]";
        auto result = edit(1007, initialRevision, operations, {});
        auto* data = result.getDynamicObject() ? result.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
        auto* mappings = data ? data->getProperty("mappings").getDynamicObject() : nullptr;
        auto* survivors = mappings ? mappings->getProperty("survivors").getArray() : nullptr;
        auto* invalidated = mappings ? mappings->getProperty("invalidated").getArray() : nullptr;
        check(data && data->getProperty("status").toString() == "applied",
            "dispatched move/delete must acknowledge reconciled GUI state");
        check(hasSurvivor(survivors, { 0 }, { 0 }) && hasSurvivor(survivors, { 4, 3 }, { 3, 2 })
                && hasSurvivor(survivors, { 5, 0 }, { 4, 0 }),
            "survivor mappings must identify moved root state and rebased child identities");
        check(hasInvalidated(invalidated, { 2 }) && hasInvalidated(invalidated, { 4, 2 }),
            "removed root and selected-child objects must be listed as invalidated pre-state identities");

        auto rootAfter = read(1008, "[]");
        auto firstAfter = read(1009, "[3]");
        auto secondAfter = read(1010, "[4]");
        check(revisionOf(rootAfter).isNotEmpty() && data
                && data->getProperty("revision").toString() == revisionOf(rootAfter),
            "applied response revision must match the live root revision");
        check(recordsOf(rootAfter) && recordsOf(rootAfter)->size() == 16
                && hasConnection(recordsOf(rootAfter), 0, 1) && !hasConnection(recordsOf(rootAfter), 2, 3),
            "root move and removal must preserve the unrelated state wire and delete only the target's incident wire");
        check(recordsOf(firstAfter) && recordsOf(firstAfter)->size() == 5
                && hasConnection(recordsOf(firstAfter), 0, 1)
                && recordsOf(secondAfter) && recordsOf(secondAfter)->size() == 4
                && hasConnection(recordsOf(secondAfter), 0, 1),
            "only the selected equal-named child must lose its target and cord while its sibling remains intact");
        editor->pd->lockAudioThread();
        bool const rootIsDirty = canvas->patch.getRawPointer()->gl_dirty != 0;
        editor->pd->unlockAudioThread();
        check(rootIsDirty, "scoped move and delete must mark the still-open root patch dirty");
        canvas->performSynchronise();
        check(canvas->objects.size() == 5, "the visible root canvas must retain its sibling subpatches and surviving objects");

        sendObject(1011, {}, 0, "bang", {});
        sendObject(1012, { 3 }, 0, "bang", {});
        sendObject(1013, { 4 }, 0, "bang", {});
        check(printLines.contains("root-state: 42") && printLines.contains("first-state: 77")
                && printLines.contains("second-state: 88"),
            "root and both child [f] values must survive movement, targeted deletion, and sibling rebasing");
        editor->pd->lockAudioThread();
        bool const dspEnabledAfterEdit = canvas_dspstate != 0;
        editor->pd->unlockAudioThread();
        check(dspEnabledAfterEdit, "deleting a child object must preserve the enabled DSP state");
        String statePrints;
        for (auto const& line : printLines) {
            if (statePrints.isNotEmpty())
                statePrints += " | ";
            statePrints += line;
        }
        std::cout << "[canvas-move-delete] visible-root-objects=" << canvas->objects.size()
                  << " dsp=" << dspEnabledBeforeEdit << "->" << dspEnabledAfterEdit
                  << " state-prints=" << statePrints
                  << " source=" << canvas->patch.getCanvasContent() << std::endl;

        closeExportFixture(canvas, "move-delete-root");
        send(1014, "{\"version\":1,\"request_id\":1014,\"operation\":\"set_generation\",\"generation\":\"generation-1\",\"root_receiver\":\"" + rootReceiver + "\"}", true, {}, {});
        if (initialDsp == 0)
            editor->pd->releaseDSP();
        editor->pd->lockAudioThread();
        bool const dspRestored = (canvas_dspstate != 0) == (initialDsp != 0);
        editor->pd->unlockAudioThread();
        check(dspRestored, "test fixture must restore the previous DSP-on condition at teardown");
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-move-delete] fixture closed; root receiver and DSP state restored" << std::endl;
    }

    void testCanvasSubpatchDeletionAndRebasing()
    {
        beginTest("Subpatch deletion and rebasing");
        flushDebugQueue();
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        String const fixture = "#N canvas 100 100 420 300 12;\n"
            "#X obj 10 10 f 0;\n#X obj 100 10 print root-state;\n"
            "#N canvas 0 0 240 160 gone 0;\n#X obj 10 10 inlet;\n#X obj 40 40 + 1;\n#X obj 40 80 outlet;\n"
            "#N canvas 0 0 120 80 inner 0;\n#X obj 10 10 print gone-descendant;\n#X restore 100 100 pd inner;\n"
            "#X connect 0 0 1 0;\n#X connect 1 0 2 0;\n#X restore 150 100 pd gone;\n"
            "#X obj 100 50 print deleted-output;\n"
            "#N canvas 0 0 240 160 twin 0;\n#X obj 10 10 f 0;\n#X obj 90 10 print sibling-state;\n"
            "#N canvas 0 0 120 80 deep 0;\n#X obj 10 10 print deep-survivor;\n#X restore 100 100 pd deep;\n"
            "#X connect 0 0 1 0;\n#X restore 270 100 pd twin;\n"
            "#X connect 0 0 1 0;\n#X connect 0 0 2 0;\n#X connect 2 0 3 0;\n";
        auto* canvas = openBoundExportFixture(fixture, "subpatch-delete-root");
        check(canvas != nullptr, "subpatch-deletion fixture must open");
        if (!canvas) {
            expectedReplies = previousReplies;
            responses = previousResponses;
            beginTest("Versioned receiver and generation registration");
            return;
        }

        auto send = [this](int const id, String const& json, bool const ok, String const& error, String const& status) {
            var request;
            check(JSON::parse(json, request).wasOk(), "subpatch deletion request must parse");
            consoleBegin();
            sendRequest(request, id, ok, error);
            if (status.isNotEmpty())
                expectedReplies.back().successStatus = status;
            flushDebugQueue();
            check(responses.size() == 1, "subpatch deletion request must return one correlated reply");
            checkConsoleReply(0, id, ok, error);
            auto result = responses.isEmpty() ? var() : responses[0];
            std::cout << "[canvas-subpatch-delete] request=" << json << " reply=" << JSON::toString(result, true) << std::endl;
            return result;
        };
        auto read = [&](int const id, String const& path) {
            return send(id, "{\"version\":1,\"request_id\":" + String(id)
                + ",\"operation\":\"read_canvas\",\"generation\":\"subpatch-delete-generation\",\"canvas_path\":" + path + "}", true, {}, {});
        };
        auto revisionOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("revision").toString() : String();
        };
        auto recordsOf = [](var const& response) {
            auto* envelope = response.getDynamicObject();
            auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
            return data ? data->getProperty("records").getArray() : nullptr;
        };
        auto edit = [&](int const id, String const& revision, String const& edits, String const& expectedError) {
            return send(id, "{\"version\":1,\"request_id\":" + String(id)
                + ",\"operation\":\"edit_canvas\",\"generation\":\"subpatch-delete-generation\",\"expected_revision\":\""
                + revision + "\",\"edits\":" + edits + "}", expectedError.isEmpty(), expectedError, {});
        };
        auto sendObject = [&](int const id, Array<int> const& path, int const ordinal, String const& selector, Array<var> const& atoms) {
            auto const request = makeSendObjectRequest(id, "subpatch-delete-generation", path, ordinal, selector, atoms);
            return send(id, JSON::toString(request, true), true, {}, "invoked");
        };
        auto sameAddress = [](var const& value, std::initializer_list<int> expected) {
            auto const* address = value.getArray();
            if (!address || address->size() != static_cast<int>(expected.size()))
                return false;
            int index = 0;
            for (auto const ordinal : expected)
                if (static_cast<int>((*address)[index++]) != ordinal)
                    return false;
            return true;
        };
        auto hasSurvivor = [&](Array<var> const* mappings, std::initializer_list<int> before, std::initializer_list<int> after) {
            if (!mappings)
                return false;
            for (auto const& value : *mappings) {
                auto* entry = value.getDynamicObject();
                if (entry && sameAddress(entry->getProperty("before"), before) && sameAddress(entry->getProperty("after"), after))
                    return true;
            }
            return false;
        };
        auto hasInvalidated = [&](Array<var> const* mappings, std::initializer_list<int> address) {
            if (!mappings)
                return false;
            for (auto const& value : *mappings) {
                if (sameAddress(value, address))
                    return true;
                auto* entry = value.getDynamicObject();
                if (!entry || !static_cast<bool>(entry->getProperty("descendants")))
                    continue;
                if (sameAddress(entry->getProperty("before"), address))
                    return true;
                auto const* prefix = entry->getProperty("before").getArray();
                if (!prefix || prefix->size() >= static_cast<int>(address.size()))
                    continue;
                int index = 0;
                bool matches = true;
                for (auto const ordinal : address) {
                    if (index == prefix->size())
                        break;
                    matches = matches && static_cast<int>((*prefix)[index++]) == ordinal;
                }
                if (matches)
                    return true;
            }
            return false;
        };
        auto hasConnection = [](Array<var> const* records, int const source, int const target) {
            if (!records)
                return false;
            for (auto const& value : *records) {
                auto* record = value.getDynamicObject();
                if (record && record->getProperty("kind").toString() == "connection"
                    && static_cast<int>(record->getProperty("source")) == source
                    && static_cast<int>(record->getProperty("target")) == target)
                    return true;
            }
            return false;
        };

        send(1100, "{\"version\":1,\"request_id\":1100,\"operation\":\"set_generation\",\"generation\":\"subpatch-delete-generation\",\"root_receiver\":\"subpatch-delete-root\"}", true, {}, {});
        auto initial = read(1101, "[]");
        auto goneBefore = read(1102, "[2]");
        auto siblingBefore = read(1103, "[4]");
        String const initialRevision = revisionOf(initial);
        auto* initialRecords = recordsOf(initial);
        check(initialRevision.length() == 64 && initialRecords && recordsOf(goneBefore)
                && recordsOf(siblingBefore),
            "subpatch fixture must expose both target descendants and its addressable sibling");
        editor->pd->lockAudioThread();
        auto* rootBeforeDelete = canvas->patch.getRawPointer();
        canvas_dirty(rootBeforeDelete, 0);
        bool const rootWasClean = rootBeforeDelete->gl_dirty == 0;
        editor->pd->unlockAudioThread();
        check(rootWasClean, "subpatch fixture must be clean before the deletion preflight");

        sendObject(1104, {}, 0, "float", { 55 });
        sendObject(1105, { 4 }, 0, "float", { 66 });
        check(printLines.contains("root-state: 55") && printLines.contains("sibling-state: 66"),
            "stateful root and surviving-sibling f objects must receive values before deletion");
        printLines.clear();
        printBuffer.clear();

        String const deletedDescendant = "[{\"op\":\"remove\",\"canvas_path\":[],\"target\":2},"
            "{\"op\":\"move\",\"canvas_path\":[2],\"target\":0,\"x\":30,\"y\":30}]";
        edit(1106, initialRevision, deletedDescendant, "CanvasNotFound");
        auto unchanged = read(1107, "[]");
        bool const unchangedStructure = initialRecords && recordsOf(unchanged)
            && JSON::toString(var(*recordsOf(unchanged)), true) == JSON::toString(var(*initialRecords), true);
        check(revisionOf(unchanged) == initialRevision && unchangedStructure,
            "a batch using a deleted descendant must leave the complete root and its cords unchanged");

        editor->pd->lockAudioThread();
        int const initialDsp = canvas_dspstate;
        editor->pd->unlockAudioThread();
        editor->pd->startDSP();
        editor->pd->lockAudioThread();
        bool const dspEnabledBeforeDelete = canvas_dspstate != 0;
        editor->pd->unlockAudioThread();
        check(dspEnabledBeforeDelete, "DSP must be enabled before subpatch deletion");

        auto result = edit(1108, initialRevision, "[{\"op\":\"remove\",\"canvas_path\":[],\"target\":2}]", {});
        auto* envelope = result.getDynamicObject();
        auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
        auto* mappings = data ? data->getProperty("mappings").getDynamicObject() : nullptr;
        auto* survivors = mappings ? mappings->getProperty("survivors").getArray() : nullptr;
        auto* invalidated = mappings ? mappings->getProperty("invalidated").getArray() : nullptr;
        check(data && data->getProperty("status").toString() == "applied",
            "subpatch destruction must acknowledge the reconciled surviving GUI");
        check(hasSurvivor(survivors, { 4 }, { 3 }) && hasSurvivor(survivors, { 4, 0 }, { 3, 0 })
                && hasSurvivor(survivors, { 4, 2 }, { 3, 2 }) && hasSurvivor(survivors, { 4, 2, 0 }, { 3, 2, 0 }),
            "survivor mappings must rebase the sibling, its objects, and its nested canvas");
        check(hasInvalidated(invalidated, { 2 }) && hasInvalidated(invalidated, { 2, 0 })
                && hasInvalidated(invalidated, { 2, 3 }) && hasInvalidated(invalidated, { 2, 3, 0 }),
            "invalidated mappings must cover the deleted subpatch and its nested descendants");

        auto rootAfter = read(1109, "[]");
        auto siblingAfter = read(1110, "[3]");
        auto deepAfter = read(1111, "[3,2]");
        check(data && data->getProperty("revision").toString() == revisionOf(rootAfter),
            "applied reply must carry the recomputed post-deletion root revision");
        check(recordsOf(rootAfter) && recordsOf(rootAfter)->size() == 13
                && hasConnection(recordsOf(rootAfter), 0, 1)
                && !hasConnection(recordsOf(rootAfter), 0, 2) && !hasConnection(recordsOf(rootAfter), 2, 3),
            "deleting a parent subpatch must remove only its incident parent cords");
        check(recordsOf(siblingAfter) && recordsOf(siblingAfter)->size() == 7
                && recordsOf(deepAfter) && recordsOf(deepAfter)->size() == 2,
            "the rebased sibling and its nested child must remain readable at their new paths");
        send(1112, "{\"version\":1,\"request_id\":1112,\"operation\":\"read_canvas\",\"generation\":\"subpatch-delete-generation\",\"canvas_path\":[4]}",
            false, "CanvasNotFound", {});
        editor->pd->lockAudioThread();
        bool const rootIsDirty = canvas->patch.getRawPointer()->gl_dirty != 0;
        editor->pd->unlockAudioThread();
        check(rootIsDirty, "subpatch deletion must mark the root patch dirty");
        canvas->performSynchronise();
        check(canvas->objects.size() == 4, "visible root structure must retain the surviving sibling after deleting one subpatch");
        String const savedOutput = canvas->patch.getCanvasContent();
        check(savedOutput.contains("#X restore 270 100 pd twin;")
                && savedOutput.contains("print deep-survivor")
                && !savedOutput.contains("print gone-descendant")
                && !savedOutput.contains("pd gone"),
            "persisted Pd source must omit the deleted subtree and retain the sibling subtree");

        sendObject(1113, {}, 0, "bang", {});
        sendObject(1114, { 3 }, 0, "bang", {});
        check(printLines.contains("root-state: 55") && printLines.contains("sibling-state: 66"),
            "root and rebased sibling [f] state must survive whole-subpatch deletion");
        editor->pd->lockAudioThread();
        bool const dspEnabledAfterDelete = canvas_dspstate != 0;
        editor->pd->unlockAudioThread();
        check(dspEnabledAfterDelete, "whole-subpatch deletion must preserve DSP-on state");
        String statePrints;
        for (auto const& line : printLines) {
            if (statePrints.isNotEmpty())
                statePrints += " | ";
            statePrints += line;
        }
        std::cout << "[canvas-subpatch-delete] visible-root-objects=" << canvas->objects.size()
                  << " dsp=" << dspEnabledBeforeDelete << "->" << dspEnabledAfterDelete
                  << " state-prints=" << statePrints
                  << " source=" << savedOutput << std::endl;

        closeExportFixture(canvas, "subpatch-delete-root");
        send(1115, "{\"version\":1,\"request_id\":1115,\"operation\":\"set_generation\",\"generation\":\"generation-1\",\"root_receiver\":\"" + rootReceiver + "\"}", true, {}, {});
        if (initialDsp == 0)
            editor->pd->releaseDSP();
        editor->pd->lockAudioThread();
        bool const dspRestored = (canvas_dspstate != 0) == (initialDsp != 0);
        editor->pd->unlockAudioThread();
        check(dspRestored, "subpatch fixture must restore the prior DSP condition at teardown");
        expectedReplies = previousReplies;
        responses = previousResponses;
        std::cout << "[canvas-subpatch-delete] fixture closed; root receiver and DSP state restored" << std::endl;
    }

    var sendTabDeleteRequest(int const requestId, String const& json, bool const ok, String const& errorCode)
    {
        var request;
        check(JSON::parse(json, request).wasOk(), "tab-deletion receiver request must parse");
        consoleBegin();
        sendRequest(request, requestId, ok, errorCode);
        flushDebugQueue();
        check(responses.size() == 1, "tab-deletion request must return one correlated reply");
        checkConsoleReply(0, requestId, ok, errorCode);
        auto result = responses.isEmpty() ? var() : responses[0];
        if (requestId == 1202 || requestId == 1212) {
            Canvas* selectedTab = nullptr;
            for (auto* pluginEditor : editor->pd->getEditors())
                for (auto* canvas : pluginEditor->getTabComponent().getCanvases())
                    if (canvas && tabDeleteTargetPatch && canvas->patch == *tabDeleteTargetPatch)
                        selectedTab = canvas;
            bool registered = false;
            for (auto const& patch : editor->pd->patches)
                if (patch && tabDeleteTargetPatch && patch.get() == tabDeleteTargetPatch.get())
                    registered = true;
            bool visible = false;
            editor->pd->lockAudioThread();
            if (tabDeleteTargetPatch) {
                if (auto* nativeCanvas = tabDeleteTargetPatch->getRawPointer())
                    visible = nativeCanvas->gl_havewindow;
            }
            editor->pd->unlockAudioThread();
            tabDeleteTargetTab = selectedTab;
            bool const inOriginalEditor = selectedTab && selectedTab->editor == tabDeleteOriginalEditor.getComponent();
            check(selectedTab && registered && visible && inOriginalEditor,
                "stale rejection must have restored the visible tab and registration in its original editor before reply receipt");
            std::cout << "[canvas-tab-delete] rejection-reply-state request=" << requestId
                      << " tab=" << (selectedTab != nullptr) << " patch=" << registered
                      << " visible=" << visible << " original-editor=" << inOriginalEditor << std::endl;
        }
        std::cout << "[canvas-tab-delete] request=" << json << " reply=" << JSON::toString(result, true) << std::endl;
        return result;
    }

    var readTabDeleteCanvas(int const requestId, String const& path)
    {
        return sendTabDeleteRequest(requestId, "{\"version\":1,\"request_id\":" + String(requestId)
                + ",\"operation\":\"read_canvas\",\"generation\":\"tab-delete-generation\",\"canvas_path\":" + path + "}",
            true, {});
    }

    static String tabDeleteRevisionOf(var const& response)
    {
        auto* envelope = response.getDynamicObject();
        auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
        return data ? data->getProperty("revision").toString() : String();
    }

    static Array<var>* tabDeleteRecordsOf(var const& response)
    {
        auto* envelope = response.getDynamicObject();
        auto* data = envelope ? envelope->getProperty("data").getDynamicObject() : nullptr;
        return data ? data->getProperty("records").getArray() : nullptr;
    }

    void testSubpatchTabDeletionAndRegistration(String const& previousGeneration, String const& previousReceiver)
    {
        beginTest("Subpatch tab deletion and registration");
        flushDebugQueue();
        String const previousRootReceiver = rootReceiver;
        String const fixtureReceiver = "tab-delete-root";
        String const rebindFixtureReceiver = "tab-delete-rebind-root";
        int const tabsBeforeFixture = [&] {
            int count = 0;
            for (auto* pluginEditor : editor->pd->getEditors())
                count += pluginEditor->getTabComponent().getCanvases().size();
            return count;
        }();
        int const patchesBeforeFixture = editor->pd->patches.size();
        int invalidPatchesBeforeFixture = 0;
        for (auto const& patch : editor->pd->patches)
            if (!patch || patch->getRawPointer() == nullptr)
                ++invalidPatchesBeforeFixture;
        String const fixture = "#N canvas 100 100 400 260 12;\n"
            "#N canvas 0 0 220 140 twin 0;\n#X obj 20 20 print first-child;\n#X restore 40 50 pd twin;\n"
            "#N canvas 0 0 220 140 twin 0;\n#X obj 20 20 print second-child;\n#X restore 180 50 pd twin;\n";
        auto* rootCanvas = openBoundExportFixture(fixture, fixtureReceiver);
        rootReceiver = previousRootReceiver;
        check(rootCanvas != nullptr, "tab-deletion root fixture must open");
        if (!rootCanvas) {
            return;
        }
        auto* rebindRootCanvas = openBoundExportFixture(fixture, rebindFixtureReceiver);
        rootReceiver = previousRootReceiver;
        check(rebindRootCanvas != nullptr, "same-hash root-rebind fixture must open");
        if (!rebindRootCanvas) {
            closeExportFixture(rootCanvas, fixtureReceiver);
            return;
        }
        rebindRootCanvas->patch.setTitle(rootCanvas->patch.getTitle());
        tabDeleteRebindRoot = rebindRootCanvas;
        tabDeleteRebindPatch = rebindRootCanvas->patch;
        tabDeleteRebindReceiver = rebindFixtureReceiver;

        bool childrenResolved = false;
        editor->pd->lockAudioThread();
        auto* rootPd = rootCanvas->patch.getRawPointer();
        auto* firstBox = rootPd ? rootPd->gl_list : nullptr;
        auto* secondBox = firstBox ? firstBox->g_next : nullptr;
        if (firstBox && secondBox && pd_class(&firstBox->g_pd) == canvas_class && pd_class(&secondBox->g_pd) == canvas_class) {
            auto* firstPdChild = reinterpret_cast<t_canvas*>(firstBox);
            auto* secondPdChild = reinterpret_cast<t_canvas*>(secondBox);
            tabDeleteTargetPatch = new pd::Patch(pd::WeakReference(firstPdChild, editor->pd), editor->pd, false);
            tabDeleteSiblingPatch = new pd::Patch(pd::WeakReference(secondPdChild, editor->pd), editor->pd, false);
            childrenResolved = true;
        }
        editor->pd->unlockAudioThread();
        check(childrenResolved && tabDeleteTargetPatch && tabDeleteSiblingPatch,
            "same-name child fixture must resolve two distinct native canvases");

        Canvas* firstTab = tabDeleteTargetPatch ? editor->getTabComponent().openPatch(tabDeleteTargetPatch, false, true) : nullptr;
        Canvas* secondTab = tabDeleteSiblingPatch ? editor->getTabComponent().openPatch(tabDeleteSiblingPatch, false, true) : nullptr;
        check(firstTab && secondTab && firstTab != secondTab,
            "both equal-named native children must open as separate tabs");
        tabDeleteRoot = rootCanvas;
        tabDeleteTargetTab = firstTab;
        tabDeleteSiblingTab = secondTab;
        tabDeleteOriginalEditor = firstTab ? firstTab->editor : nullptr;
        tabDeleteBaselineTabs = tabsBeforeFixture + 2;
        tabDeleteBaselinePatches = patchesBeforeFixture + 2;
        tabDeleteTabsBeforeFixture = tabsBeforeFixture;
        tabDeletePatchesBeforeFixture = patchesBeforeFixture;
        tabDeleteInvalidPatchesBeforeFixture = invalidPatchesBeforeFixture;
        tabDeletePreviousGeneration = previousGeneration;
        tabDeletePreviousRootReceiver = previousReceiver;
        tabDeleteReceiver = fixtureReceiver;
        tabDeleteFixturePending = true;
        bool const callbackPosted = MessageManager::callAsync([this] { runSubpatchTabDeletion(); });
        check(callbackPosted, "tab deletion must start after the child-open callbacks are drained");
    }

    void runSubpatchTabDeletion()
    {
        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        int tabsAfterOpen = 0;
        for (auto* pluginEditor : editor->pd->getEditors())
            tabsAfterOpen += pluginEditor->getTabComponent().getCanvases().size();
        check(tabsAfterOpen == tabDeleteBaselineTabs + 2 && editor->pd->patches.size() == tabDeleteBaselinePatches + 2,
            "opening both children must register exactly two tab and Pd patch entries");

        sendTabDeleteRequest(1200, "{\"version\":1,\"request_id\":1200,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteReceiver + "\"}", true, {});
        auto before = readTabDeleteCanvas(1201, "[]");
        String const revision = tabDeleteRevisionOf(before);
        auto* rootRecords = tabDeleteRecordsOf(before);
        check(revision.length() == 64 && rootRecords && rootRecords->size() == 7,
            "tab-deletion fixture must contain two distinct equal-named children");

        sendTabDeleteRequest(1203, "{\"version\":1,\"request_id\":1203,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteRebindReceiver + "\"}", true, {});
        auto sameHashRoot = readTabDeleteCanvas(1204, "[]");
        String const sameHashRevision = tabDeleteRevisionOf(sameHashRoot);
        tabDeleteRebindRevision = sameHashRevision;
        check(sameHashRevision == revision,
            "replacement root fixture must have the identical structural revision before rebind");
        sendTabDeleteRequest(1206, "{\"version\":1,\"request_id\":1206,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteReceiver + "\"}", true, {});

        tabDeleteInterleaveHookRan = false;
        pd::setCanvasEditInterleaveHookForTesting(&DirectDebugApiTest::forceStaleGenerationAfterTabClose, this);
        auto rejectedEdit = sendTabDeleteRequest(1202, "{\"version\":1,\"request_id\":1202,\"operation\":\"edit_canvas\",\"generation\":\"tab-delete-generation\",\"expected_revision\":\""
                + revision + "\",\"edits\":[{\"op\":\"remove\",\"canvas_path\":[],\"target\":0}]}",
            false, "StaleGeneration");
        pd::setCanvasEditInterleaveHookForTesting(nullptr, nullptr);
        check(tabDeleteInterleaveHookRan, "the test hook must change generation between tab close and second preflight");
        auto* staleError = rejectedEdit.getDynamicObject() ? rejectedEdit.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
        check(staleError && staleError->getProperty("code").toString() == "StaleGeneration"
                && !rejectedEdit.getDynamicObject()->hasProperty("data"),
            "the deterministic interleaving must reject before native deletion");

        sendTabDeleteRequest(1207, "{\"version\":1,\"request_id\":1207,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteReceiver + "\"}", true, {});
        auto rootAfterRejection = readTabDeleteCanvas(1208, "[]");
        auto firstChildAfterRejection = readTabDeleteCanvas(1209, "[0]");
        auto secondChildAfterRejection = readTabDeleteCanvas(1210, "[1]");
        auto* firstRecordsAfterRejection = tabDeleteRecordsOf(firstChildAfterRejection);
        auto* secondRecordsAfterRejection = tabDeleteRecordsOf(secondChildAfterRejection);
        auto* rootRecordsAfterRejection = tabDeleteRecordsOf(rootAfterRejection);
        check(tabDeleteRevisionOf(rootAfterRejection) == revision && rootRecordsAfterRejection && rootRecordsAfterRejection->size() == 7,
            "stale rejection must leave root revision and both subpatch objects unchanged");
        check(firstRecordsAfterRejection && firstRecordsAfterRejection->size() == 2
                && (*firstRecordsAfterRejection)[1].getDynamicObject()->getProperty("text").toString() == "print first-child",
            "stale rejection must leave the selected subpatch contents intact");
        check(secondRecordsAfterRejection && secondRecordsAfterRejection->size() == 2
                && (*secondRecordsAfterRejection)[1].getDynamicObject()->getProperty("text").toString() == "print second-child",
            "stale rejection must leave the same-name sibling intact");

        sendTabDeleteRequest(1211, "{\"version\":1,\"request_id\":1211,\"operation\":\"set_generation\",\"generation\":\"" + tabDeletePreviousGeneration
                + "\",\"root_receiver\":\"" + tabDeletePreviousRootReceiver + "\"}", true, {});
        expectedReplies = previousReplies;
        responses = previousResponses;
        tabDeleteRevision = revision;
        bool const callbackPosted = MessageManager::callAsync([this] { continueAfterStaleSubpatchTabDeletion(); });
        check(callbackPosted, "stale-rejection assertions must run after the restoration callback");
    }

    void continueAfterStaleSubpatchTabDeletion()
    {
        Canvas* restoredTargetTab = nullptr;
        bool targetPatchRestored = false;
        bool siblingPatchRemains = false;
        for (auto* pluginEditor : editor->pd->getEditors())
            for (auto* canvas : pluginEditor->getTabComponent().getCanvases())
                if (canvas && tabDeleteTargetPatch && canvas->patch == *tabDeleteTargetPatch)
                    restoredTargetTab = canvas;
        for (auto const& patch : editor->pd->patches) {
            if (!patch)
                continue;
            if (tabDeleteTargetPatch && patch.get() == tabDeleteTargetPatch.get())
                targetPatchRestored = true;
            if (tabDeleteSiblingPatch && patch.get() == tabDeleteSiblingPatch.get())
                siblingPatchRemains = true;
        }
        tabDeleteTargetTab = restoredTargetTab;
        int tabsAfterRejection = 0;
        for (auto* pluginEditor : editor->pd->getEditors())
            tabsAfterRejection += pluginEditor->getTabComponent().getCanvases().size();
        check(restoredTargetTab && tabDeleteSiblingTab.getComponent() && targetPatchRestored && siblingPatchRemains,
            "post-close stale rejection must restore the selected tab and preserve its sibling");
        check(tabsAfterRejection == tabDeleteBaselineTabs + 2 && editor->pd->patches.size() == tabDeleteBaselinePatches + 2,
            "stale rejection must restore both child tabs and Patch registrations");
        check(tabDeleteTargetPatch && tabDeleteTargetPatch->getRawPointer() != nullptr,
            "stale rejection must leave the selected child weak reference valid");
        std::cout << "[canvas-tab-delete] stale-rejection restored-tabs=" << tabsAfterRejection
                  << " restored-patches=" << editor->pd->patches.size() << std::endl;

        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        sendTabDeleteRequest(1214, "{\"version\":1,\"request_id\":1214,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteReceiver + "\"}", true, {});
        tabDeleteRootRebindHookRan = false;
        pd::setCanvasEditInterleaveHookForTesting(&DirectDebugApiTest::rebindDebugRootAfterTabClose, this);
        auto reboundEdit = sendTabDeleteRequest(1212, "{\"version\":1,\"request_id\":1212,\"operation\":\"edit_canvas\",\"generation\":\"tab-delete-generation\",\"expected_revision\":\""
                + tabDeleteRevision + "\",\"edits\":[{\"op\":\"remove\",\"canvas_path\":[],\"target\":0}]}",
            false, "StaleGeneration");
        pd::setCanvasEditInterleaveHookForTesting(nullptr, nullptr);
        check(tabDeleteRootRebindHookRan, "root-rebind hook must switch to the second root between preflights");
        auto* reboundError = reboundEdit.getDynamicObject() ? reboundEdit.getDynamicObject()->getProperty("error").getDynamicObject() : nullptr;
        check(reboundError && reboundError->getProperty("code").toString() == "StaleGeneration"
                && !reboundEdit.getDynamicObject()->hasProperty("data"),
            "a different root with the same structural hash must be rejected without dispatch");
        auto reboundRootRead = readTabDeleteCanvas(1215, "[]");
        auto* reboundRootRecords = tabDeleteRecordsOf(reboundRootRead);
        check(tabDeleteRevisionOf(reboundRootRead) == tabDeleteRebindRevision && reboundRootRecords && reboundRootRecords->size() == 7,
            "the rebound root must remain unchanged despite having the same expected revision");

        sendTabDeleteRequest(1216, "{\"version\":1,\"request_id\":1216,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteReceiver + "\"}", true, {});
        auto originalRootAfterRebind = readTabDeleteCanvas(1217, "[]");
        check(tabDeleteRevisionOf(originalRootAfterRebind) == tabDeleteRevision && tabDeleteRecordsOf(originalRootAfterRebind)->size() == 7,
            "the captured original root must also remain unchanged after identity rejection");
        sendTabDeleteRequest(1218, "{\"version\":1,\"request_id\":1218,\"operation\":\"set_generation\",\"generation\":\"" + tabDeletePreviousGeneration
                + "\",\"root_receiver\":\"" + tabDeletePreviousRootReceiver + "\"}", true, {});
        expectedReplies = previousReplies;
        responses = previousResponses;
        bool const callbackPosted = MessageManager::callAsync([this] { continueAfterRootRebind(); });
        check(callbackPosted, "root-rebind assertions must run after the restoration callback");
    }

    void continueAfterRootRebind()
    {
        Canvas* restoredTargetTab = nullptr;
        bool targetPatchRestored = false;
        bool siblingPatchRemains = false;
        for (auto* pluginEditor : editor->pd->getEditors())
            for (auto* canvas : pluginEditor->getTabComponent().getCanvases())
                if (canvas && tabDeleteTargetPatch && canvas->patch == *tabDeleteTargetPatch)
                    restoredTargetTab = canvas;
        for (auto const& patch : editor->pd->patches) {
            if (!patch)
                continue;
            if (tabDeleteTargetPatch && patch.get() == tabDeleteTargetPatch.get())
                targetPatchRestored = true;
            if (tabDeleteSiblingPatch && patch.get() == tabDeleteSiblingPatch.get())
                siblingPatchRemains = true;
        }
        tabDeleteTargetTab = restoredTargetTab;
        int tabsAfterRejection = 0;
        for (auto* pluginEditor : editor->pd->getEditors())
            tabsAfterRejection += pluginEditor->getTabComponent().getCanvases().size();
        check(restoredTargetTab && restoredTargetTab->editor == tabDeleteOriginalEditor.getComponent()
                && tabDeleteSiblingTab.getComponent() && targetPatchRestored && siblingPatchRemains,
            "same-hash root rebind rejection must restore the selected tab in its original editor and preserve its sibling");
        check(tabsAfterRejection == tabDeleteBaselineTabs + 2 && editor->pd->patches.size() == tabDeleteBaselinePatches + 2,
            "root-rebind rejection must restore both child tabs and Patch registrations");
        std::cout << "[canvas-tab-delete] root-rebind restored-tabs=" << tabsAfterRejection
                  << " restored-patches=" << editor->pd->patches.size() << std::endl;

        auto const previousReplies = expectedReplies;
        auto const previousResponses = responses;
        sendTabDeleteRequest(1219, "{\"version\":1,\"request_id\":1219,\"operation\":\"set_generation\",\"generation\":\"tab-delete-generation\",\"root_receiver\":\"" + tabDeleteReceiver + "\"}", true, {});
        auto beforeDelete = readTabDeleteCanvas(1220, "[]");
        String const revision = tabDeleteRevisionOf(beforeDelete);
        auto* recordsBeforeDelete = tabDeleteRecordsOf(beforeDelete);
        check(recordsBeforeDelete && recordsBeforeDelete->size() == 7,
            "valid deletion must start from the restored two-child root");

        auto edit = sendTabDeleteRequest(1221, "{\"version\":1,\"request_id\":1221,\"operation\":\"edit_canvas\",\"generation\":\"tab-delete-generation\",\"expected_revision\":\""
                + revision + "\",\"edits\":[{\"op\":\"remove\",\"canvas_path\":[],\"target\":0}]}",
            true, {});
        bool const targetTabImmediatelyAfterFree = tabDeleteTargetTab.getComponent() != nullptr;
        bool targetPatchImmediatelyAfterFree = false;
        for (auto const& patch : editor->pd->patches)
            if (patch && tabDeleteTargetPatch && patch.get() == tabDeleteTargetPatch.get())
                targetPatchImmediatelyAfterFree = true;
        std::cout << "[canvas-tab-delete] immediately-after-free target-tab=" << targetTabImmediatelyAfterFree
                  << " target-patch=" << targetPatchImmediatelyAfterFree
                  << " target-weak-valid=" << (tabDeleteTargetPatch && tabDeleteTargetPatch->getRawPointer() != nullptr)
                  << std::endl;
        check(!targetTabImmediatelyAfterFree && !targetPatchImmediatelyAfterFree,
            "valid deletion must close the selected tab and registration before native free");
        auto* editData = edit.getDynamicObject() ? edit.getDynamicObject()->getProperty("data").getDynamicObject() : nullptr;
        check(editData && editData->getProperty("status").toString() == "applied",
            "subpatch deletion must acknowledge only after the stale child tab is removed");
        auto rootAfter = readTabDeleteCanvas(1222, "[]");
        auto survivingChild = readTabDeleteCanvas(1223, "[0]");
        check(tabDeleteRevisionOf(rootAfter).isNotEmpty() && editData
                && editData->getProperty("revision").toString() == tabDeleteRevisionOf(rootAfter),
            "root registration and post-delete revision must remain readable");
        auto* remainingRecords = tabDeleteRecordsOf(survivingChild);
        check(remainingRecords && remainingRecords->size() == 2
                && (*remainingRecords)[0].getDynamicObject()->getProperty("name").toString() == "twin"
                && (*remainingRecords)[1].getDynamicObject()->getProperty("text").toString() == "print second-child",
            "the surviving same-name child must rebase to ordinal zero and remain readable");

        sendTabDeleteRequest(1224, "{\"version\":1,\"request_id\":1224,\"operation\":\"set_generation\",\"generation\":\"" + tabDeletePreviousGeneration
                + "\",\"root_receiver\":\"" + tabDeletePreviousRootReceiver + "\"}", true, {});
        expectedReplies = previousReplies;
        responses = previousResponses;
        tabDeleteRevision = tabDeleteRevisionOf(rootAfter);
        bool const callbackPosted = MessageManager::callAsync([this] { finishSubpatchTabDeletion(); });
        check(callbackPosted, "tab lifecycle verification callback must run after the queued Pd close callback");
    }

    void finishSubpatchTabDeletion()
    {
        auto* rootCanvas = tabDeleteRoot.getComponent();
        check(rootCanvas != nullptr, "the root tab must remain live until asynchronous child cleanup");
        bool const targetTabRemains = tabDeleteTargetTab.getComponent() != nullptr;
        bool const siblingTabRemains = tabDeleteSiblingTab.getComponent() != nullptr;
        bool targetPatchRemains = false;
        bool siblingPatchRemains = false;
        for (auto const& patch : editor->pd->patches) {
            if (!patch)
                continue;
            if (tabDeleteTargetPatch && patch.get() == tabDeleteTargetPatch.get())
                targetPatchRemains = true;
            if (tabDeleteSiblingPatch && patch.get() == tabDeleteSiblingPatch.get())
                siblingPatchRemains = true;
        }
        int tabsAfterDelete = 0;
        for (auto* pluginEditor : editor->pd->getEditors())
            tabsAfterDelete += pluginEditor->getTabComponent().getCanvases().size();
        int const patchesAfterDelete = editor->pd->patches.size();
        int invalidPatchesAfterDelete = 0;
        for (auto const& patch : editor->pd->patches)
            if (!patch || patch->getRawPointer() == nullptr)
                ++invalidPatchesAfterDelete;
        check(!targetTabRemains && siblingTabRemains,
            "deleting one subpatch must close only its own tab before native canvas free");
        check(!targetPatchRemains && siblingPatchRemains,
            "deleting one subpatch must remove only its own PluginProcessor::patches registration");
        check(tabDeleteTargetPatch && tabDeleteTargetPatch->getRawPointer() == nullptr,
            "the deleted child's Patch weak reference must be invalidated without dereferencing it");
        check(tabDeleteSiblingPatch && tabDeleteSiblingPatch->getRawPointer() != nullptr,
            "the same-name sibling's Patch weak reference must remain valid");
        check(invalidPatchesAfterDelete == tabDeleteInvalidPatchesBeforeFixture,
            "native subpatch free must not leave a newly invalid Patch wrapper registered");
        check(tabsAfterDelete == tabDeleteBaselineTabs + 1 && patchesAfterDelete == tabDeleteBaselinePatches + 1,
            "only the selected child tab and patch registration must be removed");
        check(rootCanvas && rootCanvas->patch.getRawPointer() != nullptr,
            "deleting a child tab must leave the registered root canvas alive");
        std::cout << "[canvas-tab-delete] tabs=" << tabDeleteBaselineTabs + 2 << "->" << tabsAfterDelete
                  << " patches=" << tabDeleteBaselinePatches + 2 << "->" << patchesAfterDelete
                  << " selected-tab=" << targetTabRemains << " sibling-tab=" << siblingTabRemains
                  << " selected-patch=" << targetPatchRemains << " sibling-patch=" << siblingPatchRemains
                  << " root-generation=" << tabDeleteRevision << std::endl;

        if (auto* staleTab = tabDeleteTargetTab.getComponent())
            editor->getTabComponent().closeTab(staleTab, false);
        if (auto* siblingTab = tabDeleteSiblingTab.getComponent())
            editor->getTabComponent().closeTab(siblingTab, false);
        if (rootCanvas)
            closeExportFixture(rootCanvas, "tab-delete-root");
        if (auto* rebindRoot = tabDeleteRebindRoot.getComponent())
            closeExportFixture(rebindRoot, tabDeleteRebindReceiver);
        int tabsAfterCleanup = 0;
        for (auto* pluginEditor : editor->pd->getEditors())
            tabsAfterCleanup += pluginEditor->getTabComponent().getCanvases().size();
        int invalidPatchesAfterCleanup = 0;
        for (auto const& patch : editor->pd->patches)
            if (!patch || patch->getRawPointer() == nullptr)
                ++invalidPatchesAfterCleanup;
        check(tabsAfterCleanup == tabDeleteTabsBeforeFixture && editor->pd->patches.size() == tabDeletePatchesBeforeFixture
                && invalidPatchesAfterCleanup == tabDeleteInvalidPatchesBeforeFixture,
            "child/root fixture teardown must leave the original tab and patch registrations unchanged");
        bool const markerPosted = MessageManager::callAsync([this] {
            tabDeleteFixturePending = false;
            std::cout << "[canvas-tab-delete] fixture closed; child tab and patch cleanup complete" << std::endl;
            tabDeleteRoot = nullptr;
            tabDeleteRebindRoot = nullptr;
            tabDeleteTargetTab = nullptr;
            tabDeleteSiblingTab = nullptr;
            tabDeleteOriginalEditor = nullptr;
            tabDeleteTargetPatch = nullptr;
            tabDeleteSiblingPatch = nullptr;
            tabDeleteRebindPatch = nullptr;
        });
        check(markerPosted, "tab fixture cleanup marker must be enqueued after close callbacks");
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
            if (capturingCanvasWire && responseIndex < static_cast<int>(expectedReplies.size())
                && (expectedReplies[static_cast<size_t>(responseIndex)].requestId == 300
                    || expectedReplies[static_cast<size_t>(responseIndex)].requestId == 303))
                std::cout << "[canvas-wire] id=" << expectedReplies[static_cast<size_t>(responseIndex)].requestId
                          << " encoded=" << encoded << std::endl;
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
        return "#N canvas 100 100 300 200 12;\n#X obj 20 20 print direct-root;\n#X text 20 50 comment;\n#N canvas 0 0 300 200 nested 0;\n#X obj 20 20 print direct-nested;\n#X restore 100 100 pd nested;\n#X msg 20 80 content-from-box;\n#X obj 20 110 print direct-root;\n#X connect 3 0 4 0;\n";
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
            consoleStep = 35;
            startTimer(10);
            break;
        }
        // (export) Task 2 (g)/(f): a canvas with no objects and a canvas whose
        // only object is outside the viewport both have an empty visible region
        // and must be rejected before any rendering or file write.
        case 35: {
            auto* zeroCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n", "export-zero-root");
            check(zeroCanvas != nullptr, "the zero-object export fixture must open");
            if (zeroCanvas) {
                registerExportGeneration(640, "export-zero-generation", "export-zero-root");

                File const tempDir = File::getSpecialLocation(File::tempDirectory);
                File const zeroFile = tempDir.getChildFile("plugdata-export-zero-" + String(Time::currentTimeMillis()) + ".png");
                zeroFile.deleteFile();

                consoleBegin();
                sendRequest(makeExportCanvasRequest(641, "export-zero-generation", zeroFile.getFullPathName()), 641, false, "ExportFailed");
                flushDebugQueue();
                checkConsoleReply(0, 641, false, "ExportFailed");
                checkConsoleErrorMessage(0, "canvas has no visible content");
                check(!zeroFile.existsAsFile(), "zero-object export must not create a file");

                closeExportFixture(zeroCanvas, "export-zero-root");
            }

            auto* farCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n#X obj 100 20000 print far-target;\n", "export-far-root");
            check(farCanvas != nullptr, "the offscreen-object export fixture must open");
            if (farCanvas) {
                registerExportGeneration(642, "export-far-generation", "export-far-root");
                check(exportExpectedRegion(farCanvas).isEmpty(), "the offscreen object must map outside the editor");

                File const tempDir = File::getSpecialLocation(File::tempDirectory);
                File const farFile = tempDir.getChildFile("plugdata-export-far-" + String(Time::currentTimeMillis()) + ".png");
                farFile.deleteFile();

                consoleBegin();
                sendRequest(makeExportCanvasRequest(643, "export-far-generation", farFile.getFullPathName()), 643, false, "ExportFailed");
                flushDebugQueue();
                checkConsoleReply(0, 643, false, "ExportFailed");
                checkConsoleErrorMessage(0, "canvas has no visible content");
                check(!farFile.existsAsFile(), "offscreen export must not create a file");

                closeExportFixture(farCanvas, "export-far-root");
            }

            std::cout << "[export] (g)+(f) empty visible region paths passed" << std::endl;
            consoleStep = 36;
            startTimer(10);
            break;
        }
        // (export) Task 2 (a)/(b): content that extends past the viewport is
        // clipped to the visible intersection, and re-exporting to an existing
        // path replaces its contents.
        case 36: {
            auto* clipCanvas = openBoundExportFixture(
                "#N canvas 100 100 300 200 12;\n#X obj 100 100 print clip-top;\n#X obj 100 20000 print clip-bottom;\n",
                "export-clip-root");
            check(clipCanvas != nullptr, "the clipping export fixture must open");
            if (clipCanvas) {
                registerExportGeneration(650, "export-clip-generation", "export-clip-root");

                auto const mapped = exportMappedContentRegion(clipCanvas);
                auto const visible = exportExpectedRegion(clipCanvas);
                check(!visible.isEmpty(), "the clipping fixture must have a visible region");
                check(visible != mapped, "the clipping fixture content must exceed the visible region");

                File const tempDir = File::getSpecialLocation(File::tempDirectory);
                File const clipFile = tempDir.getChildFile("plugdata-export-clip-" + String(Time::currentTimeMillis()) + ".png");
                clipFile.deleteFile();

                consoleBegin();
                sendRequest(makeExportCanvasRequest(651, "export-clip-generation", clipFile.getFullPathName()), 651, true, {});
                flushDebugQueue();
                checkExportSaved(0, true);
                checkExportFile(clipFile, visible, "clipped export");
                int64 const clippedSize = clipFile.getSize();
                int const clippedHeight = visible.getHeight();

                closeExportFixture(clipCanvas, "export-clip-root");

                // (b) Overwrite: a second export to the same path with shorter
                // content must replace the file; an appending writer would leave
                // the tall png first and the short one trailing.
                auto* overwriteCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n#X obj 100 100 print overwrite-target;\n", "export-overwrite-root");
                check(overwriteCanvas != nullptr, "the overwrite export fixture must open");
                if (overwriteCanvas) {
                    registerExportGeneration(652, "export-overwrite-generation", "export-overwrite-root");
                    auto const overwriteRegion = exportExpectedRegion(overwriteCanvas);
                    check(!overwriteRegion.isEmpty(), "the overwrite fixture must have a visible region");
                    check(overwriteRegion.getHeight() < clippedHeight, "the overwrite fixture must be shorter than the clipped fixture");

                    consoleBegin();
                    sendRequest(makeExportCanvasRequest(653, "export-overwrite-generation", clipFile.getFullPathName()), 653, true, {});
                    flushDebugQueue();
                    checkExportSaved(0, false);
                    checkExportFile(clipFile, overwriteRegion, "overwrite export");
                    check(clipFile.getSize() < clippedSize,
                        "overwrite export must replace the previous file instead of appending to it");

                    if (clipFile.existsAsFile())
                        clipFile.deleteFile();
                    closeExportFixture(overwriteCanvas, "export-overwrite-root");
                }
            }

            std::cout << "[export] (a)+(b) clipping and overwrite passed" << std::endl;
            consoleStep = 37;
            startTimer(10);
            break;
        }
        // (export) Task 2 (e): a path whose parent directory does not exist
        // fails the write and leaves nothing behind.
        case 37: {
            auto* writeCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n#X obj 100 100 print write-target;\n", "export-write-root");
            check(writeCanvas != nullptr, "the unwritable-target export fixture must open");
            if (writeCanvas) {
                registerExportGeneration(660, "export-write-generation", "export-write-root");

                File const tempDir = File::getSpecialLocation(File::tempDirectory);
                String const missingDir = tempDir.getChildFile("plugdata-export-missing-" + String(Time::currentTimeMillis())).getFullPathName();
                String const unwritablePath = missingDir + "/out.png";
                check(File::isAbsolutePath(unwritablePath) && unwritablePath.endsWithIgnoreCase(".png"),
                    "the unwritable fixture path must still pass path validation");

                consoleBegin();
                sendRequest(makeExportCanvasRequest(661, "export-write-generation", unwritablePath), 661, false, "ExportFailed");
                flushDebugQueue();
                checkConsoleReply(0, 661, false, "ExportFailed");
                checkConsoleErrorMessage(0, "failed to write png file");
                check(!File(unwritablePath).existsAsFile(), "unwritable export must not create a file");
                check(!File(missingDir).isDirectory(), "unwritable export must not create the missing directory");

                closeExportFixture(writeCanvas, "export-write-root");
            }

            std::cout << "[export] (e) unwritable target rejected ExportFailed" << std::endl;
            consoleStep = 38;
            startTimer(10);
            break;
        }
        // (export) Task 2 (c)/(d): a replacement generation invalidates the old
        // token (StaleGeneration, no file), while a destroyed root presenting
        // the active token is CanvasNotFound; closing a registered canvas
        // mid-flow is the same.
        case 38: {
            File const tempDir = File::getSpecialLocation(File::tempDirectory);
            File const staleFile = tempDir.getChildFile("plugdata-export-stale-" + String(Time::currentTimeMillis()) + ".png");
            staleFile.deleteFile();

            auto* oldCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n#X obj 100 100 print stale-old;\n", "export-stale-old");
            check(oldCanvas != nullptr, "the old-generation fixture must open");
            if (oldCanvas) {
                registerExportGeneration(670, "export-generation-old", "export-stale-old");
                closeExportFixture(oldCanvas, "export-stale-old");

                auto* newCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n#X obj 100 100 print stale-new;\n", "export-stale-new");
                check(newCanvas != nullptr, "the new-generation fixture must open");
                if (newCanvas) {
                    registerExportGeneration(671, "export-generation-new", "export-stale-new");

                    consoleBegin();
                    sendRequest(makeExportCanvasRequest(672, "export-generation-old", staleFile.getFullPathName()), 672, false, "StaleGeneration");
                    flushDebugQueue();
                    checkConsoleReply(0, 672, false, "StaleGeneration");
                    check(!staleFile.existsAsFile(), "stale-generation export must not create a file");

                    closeExportFixture(newCanvas, "export-stale-new");

                    consoleBegin();
                    sendRequest(makeExportCanvasRequest(673, "export-generation-new", staleFile.getFullPathName()), 673, false, "CanvasNotFound");
                    flushDebugQueue();
                    checkConsoleReply(0, 673, false, "CanvasNotFound");
                    check(!staleFile.existsAsFile(), "deleted-root export must not create a file");
                }
            }

            File const closedFile = tempDir.getChildFile("plugdata-export-closed-" + String(Time::currentTimeMillis()) + ".png");
            closedFile.deleteFile();

            auto* closedCanvas = openBoundExportFixture("#N canvas 100 100 300 200 12;\n#X obj 100 100 print closed-target;\n", "export-closed-root");
            check(closedCanvas != nullptr, "the closed-canvas fixture must open");
            if (closedCanvas) {
                registerExportGeneration(680, "export-closed-generation", "export-closed-root");
                closeExportFixture(closedCanvas, "export-closed-root");

                consoleBegin();
                sendRequest(makeExportCanvasRequest(681, "export-closed-generation", closedFile.getFullPathName()), 681, false, "CanvasNotFound");
                flushDebugQueue();
                checkConsoleReply(0, 681, false, "CanvasNotFound");
                check(!closedFile.existsAsFile(), "closed-canvas export must not create a file");
            }

            std::cout << "[export] (c)+(d) stale generation and closed canvas passed" << std::endl;
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

        testSubpatchTabDeletionAndRegistration("generation-concurrency", "concurrency-root");
        consoleStep = 18;
        startTimer(10000);
    }

    void finishConsole()
    {
        if (consoleFinished)
            return;
        consoleFinished = true;
        stopTimer();

        check(!tabDeleteFixturePending,
            "subpatch tab verification and fixture cleanup must finish before Direct Debug API teardown");

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
    Component::SafePointer<Canvas> tabDeleteRoot;
    Component::SafePointer<Canvas> tabDeleteRebindRoot;
    Component::SafePointer<Canvas> tabDeleteTargetTab;
    Component::SafePointer<Canvas> tabDeleteSiblingTab;
    Component::SafePointer<PluginEditor> tabDeleteOriginalEditor;
    pd::Patch::Ptr tabDeleteTargetPatch;
    pd::Patch::Ptr tabDeleteSiblingPatch;
    pd::Patch::Ptr tabDeleteRebindPatch;
    int tabDeleteBaselineTabs = 0;
    int tabDeleteBaselinePatches = 0;
    int tabDeleteTabsBeforeFixture = 0;
    int tabDeletePatchesBeforeFixture = 0;
    int tabDeleteInvalidPatchesBeforeFixture = 0;
    String tabDeleteRevision;
    String tabDeleteRebindRevision;
    String tabDeleteReceiver;
    String tabDeletePreviousGeneration;
    String tabDeletePreviousRootReceiver;
    String tabDeleteRebindReceiver;
    bool tabDeleteFixturePending = false;
    bool tabDeleteInterleaveHookRan = false;
    bool tabDeleteRootRebindHookRan = false;
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
    bool capturingCanvasWire = false;
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
