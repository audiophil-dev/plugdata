/*
 // Copyright (c) 2015-2022 Pierre Guillot and Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/CachedStringWidth.h"
#include "Dialogs/Dialogs.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include "Instance.h"
#include "Patch.h"
#include "MessageListener.h"
#include "Objects/ImplementationBase.h"
#include "Utility/SettingsFile.h"

extern "C" {

#include <g_undo.h>
#include <m_imp.h>
#include <m_class_probe.h>
#include "z_print_util.h"
}

#include "Pd/Interface.h"
#include "Setup.h"

EXTERN int sys_load_lib(t_canvas* canvas, char const* classname);

namespace pd {

namespace {
constexpr int debugProtocolVersion = 1;
constexpr int maxDebugRequestId = 16777215;
constexpr int maxEncodedDebugRequestBytes = 64 * 1024;
constexpr int maxDecodedDebugRequestBytes = 48 * 1024;
constexpr int maxEncodedDebugResponseBytes = 60 * 1024;
constexpr int maxGenerationBytes = 128;
constexpr int maxRootReceiverBytes = 128;

var makeDebugError(int const requestId, String const& code, String const& message)
{
    auto* error = new DynamicObject();
    error->setProperty("code", code);
    error->setProperty("message", message);

    auto* response = new DynamicObject();
    response->setProperty("version", debugProtocolVersion);
    response->setProperty("request_id", requestId);
    response->setProperty("ok", false);
    response->setProperty("error", var(error));
    return var(response);
}

var makeDebugSuccess(int const requestId)
{
    auto* response = new DynamicObject();
    response->setProperty("version", debugProtocolVersion);
    response->setProperty("request_id", requestId);
    response->setProperty("ok", true);
    response->setProperty("data", var(new DynamicObject()));
    return var(response);
}

var makeDebugInvocationSuccess(int const requestId)
{
    auto* data = new DynamicObject();
    data->setProperty("status", "invoked");

    auto* response = new DynamicObject();
    response->setProperty("version", debugProtocolVersion);
    response->setProperty("request_id", requestId);
    response->setProperty("ok", true);
    response->setProperty("data", var(data));
    return var(response);
}

bool hasOnlySetGenerationFields(DynamicObject const& request)
{
    auto const& properties = request.getProperties();
    if (properties.size() != 5)
        return false;

    for (auto const& [name, value] : properties) {
        ignoreUnused(value);
        auto const propertyName = name.toString();
        if (propertyName != "version" && propertyName != "request_id" && propertyName != "operation" && propertyName != "generation" && propertyName != "root_receiver")
            return false;
    }
    return true;
}

bool hasOnlySendObjectFields(DynamicObject const& request)
{
    auto const& properties = request.getProperties();
    if (properties.size() != 8)
        return false;

    for (auto const& [name, value] : properties) {
        ignoreUnused(value);
        auto const propertyName = name.toString();
        if (propertyName != "version" && propertyName != "request_id" && propertyName != "operation"
            && propertyName != "generation" && propertyName != "canvas_path" && propertyName != "object_ordinal"
            && propertyName != "selector" && propertyName != "atoms")
            return false;
    }
    return true;
}

class StrictJsonValidator {
public:
    struct Field {
        std::string_view value;
        int occurrences = 0;
    };

    explicit StrictJsonValidator(std::string_view const input)
        : text(input)
    {
    }

    bool parse()
    {
        skipWhitespace();
        if (position == text.size() || (text[position] != '{' && text[position] != '['))
            return false;

        objectRoot = text[position] == '{';
        if (!parseValue(0))
            return false;

        skipWhitespace();
        return position == text.size();
    }

    bool hasObjectRoot() const { return objectRoot; }
    Field const& getRequestId() const { return requestId; }
    Field const& getVersion() const { return version; }

private:
    static constexpr int maxNestingDepth = 128;

    static bool isDigit(char const character)
    {
        return character >= '0' && character <= '9';
    }

    static bool isHexDigit(char const character)
    {
        return isDigit(character)
            || (character >= 'a' && character <= 'f')
            || (character >= 'A' && character <= 'F');
    }

    static int hexDigitValue(char const character)
    {
        if (isDigit(character))
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        return character - 'A' + 10;
    }

    static bool propertyNameMatches(std::string_view const encoded, std::string_view const expected)
    {
        size_t expectedPosition = 0;
        for (size_t encodedPosition = 0; encodedPosition < encoded.size();) {
            int character = static_cast<unsigned char>(encoded[encodedPosition++]);
            if (character == '\\') {
                if (encodedPosition == encoded.size() || encoded[encodedPosition++] != 'u')
                    return false;

                character = 0;
                for (int digit = 0; digit < 4; ++digit)
                    character = character * 16 + hexDigitValue(encoded[encodedPosition++]);
            }

            if (expectedPosition == expected.size()
                || character != static_cast<unsigned char>(expected[expectedPosition++]))
                return false;
        }
        return expectedPosition == expected.size();
    }

    void skipWhitespace()
    {
        while (position < text.size()
            && (text[position] == ' ' || text[position] == '\t' || text[position] == '\n' || text[position] == '\r'))
            ++position;
    }

    bool consume(char const character)
    {
        if (position == text.size() || text[position] != character)
            return false;

        ++position;
        return true;
    }

    bool consume(std::string_view const value)
    {
        if (text.substr(position, value.size()) != value)
            return false;

        position += value.size();
        return true;
    }

    bool parseValue(int const depth)
    {
        if (depth > maxNestingDepth || position == text.size())
            return false;

        switch (text[position]) {
        case '{':
            return parseObject(depth);
        case '[':
            return parseArray(depth);
        case '"':
            return parseString();
        case 't':
            return consume("true");
        case 'f':
            return consume("false");
        case 'n':
            return consume("null");
        default:
            return parseNumber();
        }
    }

    bool parseObject(int const depth)
    {
        consume('{');
        skipWhitespace();
        if (consume('}'))
            return true;

        for (;;) {
            std::string_view propertyName;
            if (!parseString(&propertyName))
                return false;

            skipWhitespace();
            if (!consume(':'))
                return false;

            skipWhitespace();
            auto const valueStart = position;
            if (!parseValue(depth + 1))
                return false;
            if (depth == 0)
                captureTopLevelField(propertyName, text.substr(valueStart, position - valueStart));

            skipWhitespace();
            if (consume('}'))
                return true;
            if (!consume(','))
                return false;
            skipWhitespace();
        }
    }

    bool parseArray(int const depth)
    {
        consume('[');
        skipWhitespace();
        if (consume(']'))
            return true;

        for (;;) {
            if (!parseValue(depth + 1))
                return false;

            skipWhitespace();
            if (consume(']'))
                return true;
            if (!consume(','))
                return false;
            skipWhitespace();
        }
    }

    bool parseString(std::string_view* const contents = nullptr)
    {
        if (!consume('"'))
            return false;

        auto const contentStart = position;
        while (position < text.size()) {
            auto const character = static_cast<unsigned char>(text[position++]);
            if (character == '"') {
                if (contents)
                    *contents = text.substr(contentStart, position - contentStart - 1);
                return true;
            }
            if (character < 0x20)
                return false;
            if (character != '\\')
                continue;
            if (position == text.size())
                return false;

            auto const escaped = text[position++];
            if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b'
                || escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't')
                continue;
            if (escaped != 'u' || position + 4 > text.size())
                return false;

            bool nullEscape = true;
            for (int index = 0; index < 4; ++index) {
                if (!isHexDigit(text[position]))
                    return false;
                nullEscape = nullEscape && text[position] == '0';
                ++position;
            }
            if (nullEscape)
                return false;
        }
        return false;
    }

    void captureTopLevelField(std::string_view const propertyName, std::string_view const value)
    {
        if (propertyNameMatches(propertyName, "request_id")) {
            requestId.value = value;
            ++requestId.occurrences;
        } else if (propertyNameMatches(propertyName, "version")) {
            version.value = value;
            ++version.occurrences;
        }
    }

    bool parseNumber()
    {
        if (consume('-') && position == text.size())
            return false;

        if (consume('0')) {
            if (position < text.size() && isDigit(text[position]))
                return false;
        } else {
            if (position == text.size() || text[position] < '1' || text[position] > '9')
                return false;
            while (position < text.size() && isDigit(text[position]))
                ++position;
        }

        if (consume('.')) {
            if (position == text.size() || !isDigit(text[position]))
                return false;
            while (position < text.size() && isDigit(text[position]))
                ++position;
        }

        if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
            ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-'))
                ++position;
            if (position == text.size() || !isDigit(text[position]))
                return false;
            while (position < text.size() && isDigit(text[position]))
                ++position;
        }
        return true;
    }

    std::string_view text;
    size_t position = 0;
    bool objectRoot = false;
    Field requestId;
    Field version;
};

bool parseDebugRequestId(std::string_view const lexeme, int& requestId)
{
    if (lexeme.empty())
        return false;

    requestId = 0;
    for (auto const character : lexeme) {
        if (character < '0' || character > '9')
            return false;

        auto const digit = character - '0';
        if (requestId > (maxDebugRequestId - digit) / 10)
            return false;
        requestId = requestId * 10 + digit;
    }
    return requestId >= 1;
}
}

class ConsoleMessageHandler final : public Timer {
    Instance* instance;

public:
    explicit ConsoleMessageHandler(Instance* parent)
        : instance(parent)
    {
        startTimerHz(30);
    }

    void addMessage(void* object, String const& message, bool type)
    {
        if (consoleMessages.size()) {
            auto& [lastObject, lastMessage, lastType, lastLength, numMessages] = consoleMessages.back();
            if (object == lastObject && message == lastMessage && static_cast<int>(type) == lastType) {
                numMessages++;
            } else {
                consoleMessages.emplace_back(object, message, type, CachedStringWidth<14>::calculateStringWidth(message) + 40, 1);
            }
        } else {
            consoleMessages.emplace_back(object, message, type, CachedStringWidth<14>::calculateStringWidth(message) + 40, 1);
        }

        if (consoleMessages.size() > 800)
            consoleMessages.pop_front();
    }

    void logMessage(void* object, SmallString const& message)
    {
        pendingMessages.enqueue({ object, message, false });
    }

    void logWarning(void* object, SmallString const& warning)
    {
        pendingMessages.enqueue({ object, warning, true });
    }

    void logError(void* object, SmallString const& error)
    {
        pendingMessages.enqueue({ object, error, true });
    }

    void processPrint(void* object, char const* message)
    {
        std::function<void(SmallString const&)> const forwardMessage =
            [this, object](SmallString const& message) {
                if (message.startsWith("error")) {
                    logError(object, message.substring(7));
                } else if (message.startsWith("verbose(0):") || message.startsWith("verbose(1):")) {
                    logError(object, message.substring(12));
                } else {
                    if (message.startsWith("verbose(")) {
                        logMessage(object, message.substring(12));
                    } else {
                        logMessage(object, message);
                    }
                }
            };

        printConcatBuffer[messageLength] = '\0';

        int len = static_cast<int>(strlen(message));
        while (messageLength + len >= 2048) {
            int const d = 2048 - 1 - messageLength;
            strncat(printConcatBuffer.data(), message, d);

            // Send concatenated line to plugdata!
            forwardMessage(SmallString(printConcatBuffer.data()));

            message += d;
            len -= d;
            messageLength = 0;
            printConcatBuffer[0] = '\0';
        }

        strncat(printConcatBuffer.data(), message, len);
        messageLength += len;

        if (messageLength > 0 && printConcatBuffer[messageLength - 1] == '\n') {
            printConcatBuffer[messageLength - 1] = '\0';

            // Send concatenated line to plugdata!
            forwardMessage(SmallString(printConcatBuffer.data()));

            messageLength = 0;
        }
    }

    std::deque<std::tuple<void*, String, int, int, int>> consoleMessages;
    std::deque<std::tuple<void*, String, int, int, int>> consoleHistory;

private:
    void timerCallback() override
    {
        auto item = std::tuple<void*, SmallString, bool>();
        int numReceived = 0;
        bool newWarning = false;
        SmallString lastMessage;
        bool lastIsWarning = false;

        while (pendingMessages.try_dequeue(item)) {
            auto& [object, message, type] = item;
            addMessage(object, message.toString(), type);

            numReceived++;
            newWarning = newWarning || type;
            lastMessage = message;
            lastIsWarning = type;
        }

        // Check if any item got assigned
        if (numReceived) {
            instance->updateConsole(lastMessage, lastIsWarning, numReceived, newWarning);
        }
    }

    StackArray<char, 2048> printConcatBuffer = { };

    moodycamel::ConcurrentQueue<std::tuple<void*, SmallString, bool>> pendingMessages = moodycamel::ConcurrentQueue<std::tuple<void*, SmallString, bool>>(512);
    int messageLength = 0;
};

struct Instance::dmessage {

    dmessage(pd::Instance* instance, void* ref, SmallString const& dest, SmallString const& sel, SmallArray<pd::Atom> const& atoms)
        : object(ref, instance)
        , destination(dest)
        , selector(sel)
        , list(atoms)
    {
    }

    WeakReference object;
    SmallString destination;
    SmallString selector;
    SmallArray<pd::Atom> list;
};

struct Instance::internal {

    static void instance_multi_bang(pd::Instance* ptr, char const* recv)
    {
        ptr->enqueueGuiMessage({ SmallString("bang"), String::fromUTF8(recv) });
    }

    static void instance_multi_float(pd::Instance* ptr, char const* recv, float f)
    {
        ptr->enqueueGuiMessage({ SmallString("float"), SmallString(recv), SmallArray<Atom>(1, { f }) });
    }

    static void instance_multi_symbol(pd::Instance* ptr, char const* recv, char const* sym)
    {
        ptr->enqueueGuiMessage({ SmallString("symbol"), SmallString(recv), SmallArray<Atom>(1, ptr->generateSymbol(sym)) });
    }

    static void instance_multi_list(pd::Instance* ptr, char const* recv, int const argc, t_atom const* argv)
    {
        Message mess { SmallString("list"), SmallString(recv), SmallArray<Atom>(argc) };
        for (int i = 0; i < argc; ++i) {
            if (argv[i].a_type == A_FLOAT)
                mess.list[i] = Atom(atom_getfloat(argv + i));
            else if (argv[i].a_type == A_SYMBOL)
                mess.list[i] = Atom(atom_getsymbol(argv + i));
        }

        ptr->enqueueGuiMessage(mess);
    }

    static void instance_multi_message(pd::Instance* ptr, char const* recv, char const* msg, int const argc, t_atom const* argv)
    {
        Message mess { msg, String::fromUTF8(recv), SmallArray<Atom>(argc) };
        for (int i = 0; i < argc; ++i) {
            if (argv[i].a_type == A_FLOAT)
                mess.list[i] = Atom(atom_getfloat(argv + i));
            else if (argv[i].a_type == A_SYMBOL)
                mess.list[i] = Atom(atom_getsymbol(argv + i));
        }
        ptr->enqueueGuiMessage(mess);
    }

    static void instance_multi_noteon(pd::Instance* ptr, int const channel, int const pitch, int const velocity)
    {
        ptr->receiveNoteOn(channel, pitch, velocity);
    }

    static void instance_multi_controlchange(pd::Instance* ptr, int const channel, int const controller, int const value)
    {
        ptr->receiveControlChange(channel, controller, value);
    }

    static void instance_multi_programchange(pd::Instance* ptr, int const channel, int const value)
    {
        ptr->receiveProgramChange(channel, value);
    }

    static void instance_multi_pitchbend(pd::Instance* ptr, int const channel, int const value)
    {
        ptr->receivePitchBend(channel, value);
    }

    static void instance_multi_aftertouch(pd::Instance* ptr, int const channel, int const value)
    {
        ptr->receiveAftertouch(channel, value);
    }

    static void instance_multi_polyaftertouch(pd::Instance* ptr, int const channel, int const pitch, int const value)
    {
        ptr->receivePolyAftertouch(channel, pitch, value);
    }

    static void instance_multi_midibyte(pd::Instance* ptr, int const port, int const byte)
    {
        ptr->receiveMidiByte(port + 1, byte);
    }

    static void instance_multi_print(pd::Instance const* ptr, void* object, char const* s)
    {
        ptr->consoleMessageHandler->processPrint(object, s);
    }
};

Instance::Instance()
    : messageDispatcher(std::make_unique<MessageDispatcher>())
    , consoleMessageHandler(std::make_unique<ConsoleMessageHandler>(this))
{
    pd::Setup::initialisePd();
    objectImplementations = std::make_unique<::ObjectImplementationManager>(this);
}

Instance::~Instance()
{
    // Empty out the function queue because it could be referencing other things inside the lambda captures
    // (inside a scope so that "item" also gets fully deleted before we delete this class)
    {
        std::function<void()> item;
        while (functionQueue.try_dequeue(item)) { }
    }

    objectImplementations.reset(nullptr); // Make sure it gets deallocated before pd instance gets deleted

    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    pd_free(static_cast<t_pd*>(messageReceiver));
    pd_free(static_cast<t_pd*>(midiReceiver));
    gensym("#plugdata_print")->s_thing = nullptr; // In case any object tries to print during shutdown
    pd_free(static_cast<t_pd*>(printReceiver));
    pd_free(static_cast<t_pd*>(parameterReceiver));
    pd_free(static_cast<t_pd*>(pluginLatencyReceiver));
    pd_free(static_cast<t_pd*>(dataBufferReceiver));
    pd_free(static_cast<t_pd*>(debugReceiver));

    libpd_free_instance(static_cast<t_pdinstance*>(instance));
}

// ag: Stuff to be done after unpacking the library data on first launch.
void Instance::initialisePd(String& pdlua_version)
{
    instance = libpd_new_instance();

    libpd_set_instance(static_cast<t_pdinstance*>(instance));

    setup_lock(
        &audioLock,
        [](void* lock) {
            static_cast<CriticalSection*>(lock)->enter();
        },
        [](void* lock) {
            static_cast<CriticalSection*>(lock)->exit();
        });

    setup_weakreferences(
        [](void* instance, void* ref) {
            static_cast<pd::Instance*>(instance)->clearWeakReferences(ref);
        },
        [](void* instance, void* ref, void* weakref) {
            auto** referenceState = static_cast<pd_weak_reference**>(weakref);
            *referenceState = new pd_weak_reference(true);
            static_cast<pd::Instance*>(instance)->registerWeakReference(ref, *referenceState);
        },
        [](void* instance, void* ref, void* weakref) {
            auto** referenceState = static_cast<pd_weak_reference**>(weakref);
            static_cast<pd::Instance*>(instance)->unregisterWeakReference(ref, *referenceState);
            delete *referenceState;
        },
        [](void* ref) -> int {
            return static_cast<pd_weak_reference*>(ref)->load();
        });

    midiReceiver = pd::Setup::createMIDIHook(this, reinterpret_cast<t_plugdata_noteonhook>(internal::instance_multi_noteon), reinterpret_cast<t_plugdata_controlchangehook>(internal::instance_multi_controlchange), reinterpret_cast<t_plugdata_programchangehook>(internal::instance_multi_programchange),
        reinterpret_cast<t_plugdata_pitchbendhook>(internal::instance_multi_pitchbend), reinterpret_cast<t_plugdata_aftertouchhook>(internal::instance_multi_aftertouch), reinterpret_cast<t_plugdata_polyaftertouchhook>(internal::instance_multi_polyaftertouch),
        reinterpret_cast<t_plugdata_midibytehook>(internal::instance_multi_midibyte));

    messageReceiver = pd::Setup::createReceiver(this, "pd", reinterpret_cast<t_plugdata_banghook>(internal::instance_multi_bang), reinterpret_cast<t_plugdata_floathook>(internal::instance_multi_float), reinterpret_cast<t_plugdata_symbolhook>(internal::instance_multi_symbol),
        reinterpret_cast<t_plugdata_listhook>(internal::instance_multi_list), reinterpret_cast<t_plugdata_messagehook>(internal::instance_multi_message));

    parameterReceiver = pd::Setup::createReceiver(this, "__param", reinterpret_cast<t_plugdata_banghook>(internal::instance_multi_bang), reinterpret_cast<t_plugdata_floathook>(internal::instance_multi_float), reinterpret_cast<t_plugdata_symbolhook>(internal::instance_multi_symbol),
        reinterpret_cast<t_plugdata_listhook>(internal::instance_multi_list), reinterpret_cast<t_plugdata_messagehook>(internal::instance_multi_message));

    pluginLatencyReceiver = pd::Setup::createReceiver(this, "__latency_compensation", reinterpret_cast<t_plugdata_banghook>(internal::instance_multi_bang), reinterpret_cast<t_plugdata_floathook>(internal::instance_multi_float), reinterpret_cast<t_plugdata_symbolhook>(internal::instance_multi_symbol),
        reinterpret_cast<t_plugdata_listhook>(internal::instance_multi_list), reinterpret_cast<t_plugdata_messagehook>(internal::instance_multi_message));

    dataBufferReceiver = pd::Setup::createReceiver(this, "__to_daw_databuffer", reinterpret_cast<t_plugdata_banghook>(internal::instance_multi_bang), reinterpret_cast<t_plugdata_floathook>(internal::instance_multi_float), reinterpret_cast<t_plugdata_symbolhook>(internal::instance_multi_symbol),
        reinterpret_cast<t_plugdata_listhook>(internal::instance_multi_list), reinterpret_cast<t_plugdata_messagehook>(internal::instance_multi_message));

    debugReceiver = pd::Setup::createReceiver(this, "__pd_mcp_debug", reinterpret_cast<t_plugdata_banghook>(internal::instance_multi_bang), reinterpret_cast<t_plugdata_floathook>(internal::instance_multi_float), reinterpret_cast<t_plugdata_symbolhook>(internal::instance_multi_symbol),
        reinterpret_cast<t_plugdata_listhook>(internal::instance_multi_list), reinterpret_cast<t_plugdata_messagehook>(internal::instance_multi_message));

    // Register callback for special Pd messages
    auto gui_trigger = [](void* instance, char const* name, int const argc, t_atom* argv) {
        switch (hash(name)) {
        // NOTE: this sometimes gets called when an object does sys_vgui("destroy %s") on something other than a canvas
        // don't dereference the glist until we're sure it's a canvas
        case hash("canvas_vis"): {
            auto* inst = static_cast<Instance*>(instance);
            if (inst->initialiseIntoPluginmode)
                return;

            t_canvas* glist = reinterpret_cast<struct _glist*>(argv->a_w.w_gpointer);

            // Make sure we're not a toplevel without checking gl_owner
            for (auto* x = pd_getcanvaslist(); x; x = x->gl_next)
                if (x == glist)
                    return;

            if (atom_getfloat(argv + 1)) {
                File patchFile;
                if (canvas_isabstraction(glist)) {
                    patchFile = File(String::fromUTF8(canvas_getdir(glist)->s_name)).getChildFile(String::fromUTF8(glist->gl_name->s_name)).withFileExtension("pd");
                }

                MessageManager::callAsync([inst = juce::WeakReference(inst), patchToOpen = pd::WeakReference(glist, inst), patchFile] {
                    if (auto* pd = static_cast<PluginProcessor*>(inst.get())) {
                        PluginEditor* activeEditor = nullptr;
                        for (auto* editor : pd->getEditors()) {
                            if (editor->isActiveWindow()) {
                                activeEditor = editor;
                                break;
                            }
                        }
                        if (!activeEditor || !patchToOpen.isValid())
                            return;

                        for (auto const& patch : pd->patches) {
                            if (patch->getRawPointer() == patchToOpen.getRaw<t_glist>()) {
                                activeEditor->getTabComponent().openPatch(patch);
                                return;
                            }
                        }

                        pd::Patch::Ptr const subpatch = new pd::Patch(patchToOpen, pd, false);
                        if (patchFile.exists()) {
                            subpatch->setCurrentFile(URL(patchFile));
                        }
                        activeEditor->getTabComponent().openPatch(subpatch, false, false);
                    }
                });
            } else {
                MessageManager::callAsync([inst = juce::WeakReference(inst), glist] {
                    if (auto const* pd = static_cast<PluginProcessor*>(inst.get())) {
                        for (auto* editor : pd->getEditors()) {
                            for (auto* canvas : editor->getCanvases()) {
                                auto canvasPtr = canvas->patch.getPointer();
                                if (canvasPtr && canvasPtr.get() == glist) {
                                    canvas->editor->getTabComponent().closeTab(canvas, false);
                                    break;
                                }
                            }
                        }
                    }
                });
            }
            break;
        }
        case hash("canvas_undo_redo"): {
            auto* inst = static_cast<Instance*>(instance);
            auto const* glist = reinterpret_cast<t_canvas*>(argv->a_w.w_gpointer);
            auto const* undoName = atom_getsymbol(argv + 1);
            auto const* redoName = atom_getsymbol(argv + 2);
            MessageManager::callAsync([instance = juce::WeakReference(inst), glist, undoName, redoName] {
                if (auto* pd = static_cast<PluginProcessor*>(instance.get())) {
                    for (auto const& patch : pd->patches) {
                        if (patch->ptr.getRaw<t_canvas>() == glist) {
                            patch->updateUndoRedoState(SmallString(undoName->s_name), SmallString(redoName->s_name));
                        }
                    }
                    for (auto* editor : pd->getEditors())
                        editor->triggerAsyncUpdate();
                }
            });
            break;
        }
        case hash("canvas_title"): {
            auto* inst = static_cast<Instance*>(instance);
            auto* glist = reinterpret_cast<t_canvas*>(argv->a_w.w_gpointer);
            auto title = SmallString(atom_getsymbol(argv + 1)->s_name);
            int isDirty = atom_getfloat(argv + 2);

            int argc = 0;
            t_atom* argv = nullptr;

            canvas_setcurrent(glist);
            canvas_getargs(&argc, &argv);
            canvas_unsetcurrent(glist);

            if (argc) {
                title += " (";
                for (int i = 0; i < argc; i++) {
                    char namebuf[MAXPDSTRING];
                    atom_string(&argv[i], namebuf, MAXPDSTRING);
                    title += String::fromUTF8(namebuf);
                    if (i != argc - 1)
                        title += " ";
                }
                title += ")";
            }

            title = title.isEmpty() ? "Untitled Patcher" : title;

            MessageManager::callAsync([instance = juce::WeakReference(inst), glist, title, isDirty] {
                if (auto* pd = static_cast<PluginProcessor*>(instance.get())) {
                    for (auto const& patch : pd->patches) {
                        if (patch->ptr.getRaw<t_canvas>() == glist) {
                            patch->updateTitle(title, isDirty);
                        }
                    }
                    for (auto* editor : pd->getEditors())
                        editor->triggerAsyncUpdate();
                }
            });
            break;
        }
        case hash("openpanel"): {
#if ENABLE_TESTING
            break; // Don't open files during testing
#endif
            auto const openMode = argc >= 4 ? static_cast<int>(atom_getfloat(argv + 3)) : -1;
            static_cast<Instance*>(instance)->createPanel(atom_getfloat(argv), atom_getsymbol(argv + 1)->s_name, atom_getsymbol(argv + 2)->s_name, "callback", openMode);

            break;
        }
        case hash("elsepanel"): {
#if ENABLE_TESTING
            break; // Don't open files during testing
#endif
            static_cast<Instance*>(instance)->createPanel(atom_getfloat(argv), atom_getsymbol(argv + 1)->s_name, atom_getsymbol(argv + 2)->s_name, "symbol");
            break;
        }
        case hash("openfile"):
        case hash("openfile_open"): {
#if ENABLE_TESTING
            break; // Don't open files during testing
#endif
            auto const url = String::fromUTF8(atom_getsymbol(argv)->s_name);
            if (URL::isProbablyAWebsiteURL(url)) {
                URL(url).launchInDefaultBrowser();
            } else {
                if (File(url).exists()) {
                    File(url).startAsProcess();
                } else if (argc > 1) {
                    auto const fullPath = File(String::fromUTF8(atom_getsymbol(argv)->s_name)).getChildFile(url);
                    if (fullPath.exists()) {
                        fullPath.startAsProcess();
                    }
                }
            }

            break;
        }
        case hash("cyclone_editor"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            SmallString title;

            if (argc > 5) {
                auto owner = SmallString(atom_getsymbol(argv + 3)->s_name);
                title = SmallString(atom_getsymbol(argv + 4)->s_name);
            } else {
                title = SmallString(atom_getsymbol(argv + 3)->s_name);
            }

            auto save = [title, inst = static_cast<Instance*>(instance)](String text, uint64_t const ptr) {
                inst->lockAudioThread();
                pd_typedmess(reinterpret_cast<t_pd*>(ptr), gensym("clear"), 0, nullptr);

                // remove repeating spaces
                text = text.replace("\r\n", "\n");
                text = text.replace("\r", "\n");
                text = text.replace("\n ", "\n");
                text = text.replace(";\n", ";");
                text = text.replace("\n;", ";");
                text = text.replace(" ;", ";");
                text = text.replace("; ", ";");
                text = text.replace(",", " , ");
                text = text.replaceCharacters("\n", " ");

                while (text.contains("  ")) {
                    text = text.replace("  ", " ");
                }
                text = text.trimStart();
                auto lines = StringArray::fromTokens(text, ";", "\"");

                int count = 0;
                for (auto const& line : lines) {
                    count++;
                    auto words = StringArray::fromTokens(line, " ", "\"");

                    auto atoms = SmallArray<t_atom>();
                    atoms.reserve(words.size() + 1);

                    for (auto const& word : words) {
                        atoms.emplace_back();
                        // check if string is a valid number
                        auto charptr = word.getCharPointer();
                        auto ptr = charptr;
                        CharacterFunctions::readDoubleValue(ptr); // Removes double value from char*
                        if (*charptr == ',') {
                            SETCOMMA(&atoms.back());
                        } else if (ptr - charptr == word.getNumBytesAsUTF8() && ptr - charptr != 0) {
                            SETFLOAT(&atoms.back(), word.getFloatValue());
                        } else {
                            SETSYMBOL(&atoms.back(), inst->generateSymbol(word));
                        }
                    }

                    if (count != lines.size()) {
                        atoms.emplace_back();
                        SETSEMI(&atoms.back());
                    }

                    pd_typedmess(reinterpret_cast<t_pd*>(ptr), gensym("addline"), atoms.size(), atoms.data());
                }

                pd_typedmess(reinterpret_cast<t_pd*>(ptr), inst->generateSymbol("end"), 0, nullptr);
                inst->unlockAudioThread();
            };

            static_cast<Instance*>(instance)->showTextEditorDialog(ptr, title, save, [](uint64_t) { });
            break;
        }
        case hash("cyclone_editor_append"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            auto const text = String::fromUTF8(atom_getsymbol(argv + 1)->s_name);

            static_cast<Instance*>(instance)->addTextToTextEditor(ptr, text);
            break;
        }
        case hash("cyclone_editor_close"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            static_cast<Instance*>(instance)->hideTextEditorDialog(ptr);
            break;
        }
        case hash("coll_check_open"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            bool const open = static_cast<bool>(atom_getfloat(argv + 1));
            bool const wasOpen = static_cast<Instance*>(instance)->isTextEditorDialogShown(ptr);

            StackArray<t_atom, 2> atoms;
            SETFLOAT(&atoms[0], wasOpen);
            SETFLOAT(&atoms[1], open);

            pd_typedmess(reinterpret_cast<t_pd*>(ptr), gensym("_is_opened"), 2, atoms.data());
            break;
        }
        case hash("pdtk_textwindow_open"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            auto* inst = static_cast<Instance*>(instance);
            auto const* title = atom_getsymbol(argv + 1);

            auto save = [inst](String text, uint64_t const ptr) {
                inst->lockAudioThread();
                pd_typedmess(reinterpret_cast<t_pd*>(ptr), gensym("clear"), 0, nullptr);

                // remove repeating spaces
                text = text.replace("\r\n", "\n");
                text = text.replace("\r", "\n");
                text = text.replace("\n ", "\n");
                text = text.replace(";\n", ";");
                text = text.replace("\n;", ";");
                text = text.replace(" ;", ";");
                text = text.replace("; ", ";");
                text = text.replace(",", " , ");
                text = text.replaceCharacters("\n", " ");

                while (text.contains("  ")) {
                    text = text.replace("  ", " ");
                }
                text = text.trimStart();
                auto lines = StringArray::fromTokens(text, ";", "\"");

                int count = 0;
                for (auto const& line : lines) {
                    count++;
                    auto words = StringArray::fromTokens(line, " ", "\"");

                    auto atoms = SmallArray<t_atom>();
                    atoms.reserve(words.size() + 1);

                    for (auto const& word : words) {
                        atoms.emplace_back();
                        // check if string is a valid number
                        auto charptr = word.getCharPointer();
                        auto ptr = charptr;
                        CharacterFunctions::readDoubleValue(ptr); // Removes double value from char*
                        if (*charptr == ',') {
                            SETCOMMA(&atoms.back());
                        } else if (ptr - charptr == word.getNumBytesAsUTF8() && ptr - charptr != 0) {
                            SETFLOAT(&atoms.back(), word.getFloatValue());
                        } else {
                            SETSYMBOL(&atoms.back(), inst->generateSymbol(word));
                        }
                    }

                    if (count != lines.size()) {
                        atoms.emplace_back();
                        SETSEMI(&atoms.back());
                    }

                    pd_typedmess(reinterpret_cast<t_pd*>(ptr), gensym("addline"), atoms.size(), atoms.data());
                }

                pd_typedmess(reinterpret_cast<t_pd*>(ptr), inst->generateSymbol("notify"), 0, nullptr);
                inst->unlockAudioThread();
            };

            auto close = [inst](uint64_t const ptr) {
                inst->lockAudioThread();
                pd_typedmess(reinterpret_cast<t_pd*>(ptr), inst->generateSymbol("close"), 0, nullptr);
                inst->unlockAudioThread();
            };

            static_cast<Instance*>(instance)->showTextEditorDialog(ptr, String::fromUTF8(title->s_name), save, close);
            break;
        }
        case hash("pdtk_textwindow_doclose"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            static_cast<Instance*>(instance)->hideTextEditorDialog(ptr);
            break;
        }
        case hash("pdtk_textwindow_clear"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            static_cast<Instance*>(instance)->clearTextEditor(ptr);
            break;
        }
        case hash("pdtk_textwindow_appendatoms"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            auto const argv_start = argv + 1;

            // Create a binbuf to store the atoms
            t_binbuf* b = binbuf_new();
            binbuf_add(b, argc - 1, argv_start); // Add atoms to binbuf

            // Convert binbuf to a string
            char* text = nullptr;
            int length = 0;
            binbuf_gettext(b, &text, &length);
            if (text) {
                auto editorText = String::fromUTF8(text, length);
                editorText = editorText.replace("; ", ";\n");
                editorText = editorText.replace("\n\n", "\n");
                static_cast<Instance*>(instance)->addTextToTextEditor(ptr, editorText);
            }

            freebytes(text, length);
            binbuf_free(b);
            break;
        }
        case hash("pdtk_textwindow_raise"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            static_cast<Instance*>(instance)->raiseTextEditorDialog(ptr);
            break;
        }
        case hash("pdtk_textwindow_destroy"): {
            auto const ptr = reinterpret_cast<uint64_t>(argv->a_w.w_gpointer);
            static_cast<Instance*>(instance)->hideTextEditorDialog(ptr);
            break;
        }
        default:
            break;
        }
    };

    register_gui_triggers(static_cast<t_pdinstance*>(instance), this, gui_trigger, &MessageDispatcher::enqueueMessage);

    static bool initialised = false;
    if (!initialised) {
        // Make sure we set the maininstance when initialising objects
        // Whenever a new instance is created, the functions will be copied from this one
        libpd_set_instance(libpd_main_instance());

        set_class_prefix(gensym("else"));
        class_set_extern_dir(gensym("9.else"));
        pd::Setup::initialiseELSE();
        set_class_prefix(gensym("cyclone"));
        class_set_extern_dir(gensym("10.cyclone"));
        pd::Setup::initialiseCyclone();

        set_class_prefix(gensym("Gem"));

        class_set_extern_dir(gensym("14.gem"));
        pd::Setup::initialiseGem(ProjectInfo::appDataDir.getChildFile("Extra").getChildFile("Gem").getFullPathName().toStdString());

        class_set_extern_dir(gensym(""));
        set_class_prefix(nullptr);
        initialised = true;


        // We want to initialise pdlua separately for each instance
        auto const extra = ProjectInfo::appDataDir.getChildFile("Extra");
        StackArray<char, 1000> vers;
        vers[0] = 0;
        pd::Setup::initialisePdLua(extra.getFullPathName().getCharPointer(), vers.data(), 1000, &registerLuaClass);
        if (vers[0])
            pdlua_version = vers.data();
    }

    setThis();
    pd::Setup::initialisePdInstance();

    // ag: need to do this here to suppress noise from chatty externals
    printReceiver = pd::Setup::createPrintHook(this, reinterpret_cast<t_plugdata_printhook>(internal::instance_multi_print));
    libpd_set_verbose(0);

    set_plugdata_debugging_enabled(SettingsFile::getInstance()->getProperty<bool>("debug_connections"));
}

int Instance::getBlockSize()
{
    return libpd_blocksize();
}

void Instance::prepareDSP(int const nins, int const nouts, double const samplerate)
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_init_audio(nins, nouts, static_cast<int>(samplerate));
}

void Instance::startDSP()
{
    lockAudioThread();
    t_atom av;
    libpd_set_float(&av, 1.f);
    libpd_message("pd", "dsp", 1, &av);
    unlockAudioThread();
}

void Instance::releaseDSP()
{
    lockAudioThread();
    t_atom av;
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_set_float(&av, 0.f);
    libpd_message("pd", "dsp", 1, &av);
    unlockAudioThread();
}

void Instance::performDSP(float const* inputs, float* outputs)
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_process_raw(inputs, outputs);
}

void Instance::sendNoteOn(int const channel, int const pitch, int const velocity) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_noteon(channel - 1, pitch, velocity);
}

void Instance::sendControlChange(int const channel, int const controller, int const value) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_controlchange(channel - 1, controller, value);
}

void Instance::sendProgramChange(int const channel, int const value) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_programchange(channel - 1, value);
}

void Instance::sendPitchBend(int const channel, int const value) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_pitchbend(channel - 1, value);
}

void Instance::sendAfterTouch(int const channel, int const value) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_aftertouch(channel - 1, value);
}

void Instance::sendPolyAfterTouch(int const channel, int const pitch, int const value) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_polyaftertouch(channel - 1, pitch, value);
}

void Instance::sendSysEx(int const port, int const byte) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_sysex(port, byte);
}

void Instance::sendSysRealTime(int const port, int const byte) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_sysrealtime(port, byte);
}

void Instance::sendMidiByte(int const port, int const byte) const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_midibyte(port, byte);
}

void Instance::sendBang(char const* receiver) const
{
    if (!ProjectInfo::isStandalone && !instance)
        return;

    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_bang(receiver);
}

void Instance::sendFloat(char const* receiver, float const value) const
{
    if (!ProjectInfo::isStandalone && !instance)
        return;

    libpd_set_instance(static_cast<t_pdinstance*>(instance));

    libpd_float(receiver, value);
}

void Instance::sendSymbol(char const* receiver, char const* symbol) const
{
    if (!ProjectInfo::isStandalone && !instance)
        return;

    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    libpd_symbol(receiver, symbol);
}

void Instance::sendList(char const* receiver, SmallArray<Atom> const& list) const
{
    auto argv = SmallArray<t_atom>(list.size());
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].isFloat())
            libpd_set_float(argv.data() + i, list[i].getFloat());
        else
            libpd_set_symbol(argv.data() + i, list[i].getSymbol()->s_name);
    }
    libpd_list(receiver, static_cast<int>(list.size()), argv.data());
}

void Instance::sendTypedMessage(void* object, char const* msg, SmallArray<Atom> const& list) const
{
    if (!object)
        return;

    libpd_set_instance(static_cast<t_pdinstance*>(instance));

    auto argv = SmallArray<t_atom>(list.size());

    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].isFloat())
            libpd_set_float(argv.data() + i, list[i].getFloat());
        else
            libpd_set_symbol(argv.data() + i, list[i].getSymbol()->s_name);
    }

    pd_typedmess(static_cast<t_pd*>(object), generateSymbol(msg), static_cast<int>(list.size()), argv.data());
}

void Instance::sendMessage(char const* receiver, char const* msg, SmallArray<Atom> const& list) const
{
    sendTypedMessage(generateSymbol(receiver)->s_thing, msg, list);
}

void Instance::processSend(dmessage const& mess)
{
    if (mess.object.isValid()) {
        dispatchResolvedMessage(mess.object.getRawUnchecked<t_pd>(), mess.selector, mess.list);
    } else {
        sendMessage(mess.destination.data(), mess.selector.data(), mess.list);
    }
}

void Instance::dispatchResolvedMessage(t_pd* const object, SmallString const& selector, SmallArray<Atom> const& atoms)
{
    if (selector == "bang" && atoms.empty()) {
        pd_bang(object);
        return;
    }

    if (selector == "list") {
        auto argv = SmallArray<t_atom>(atoms.size());
        for (size_t i = 0; i < atoms.size(); ++i) {
            if (atoms[i].isFloat())
                SETFLOAT(argv.data() + i, atoms[i].getFloat());
            else if (atoms[i].isSymbol())
                SETSYMBOL(argv.data() + i, atoms[i].getSymbol());
            else
                SETFLOAT(argv.data() + i, 0.0);
        }
        pd_list(object, generateSymbol("list"), static_cast<int>(atoms.size()), argv.data());
        return;
    }

    if (selector == "float" && atoms.size() == 1 && atoms[0].isFloat()) {
        pd_float(object, atoms[0].getFloat());
        return;
    }

    if (selector == "symbol" && atoms.size() == 1 && atoms[0].isSymbol()) {
        pd_symbol(object, atoms[0].getSymbol());
        return;
    }

    sendTypedMessage(object, selector.data(), atoms);
}

void Instance::registerMessageListener(void* object, MessageListener* messageListener)
{
    messageDispatcher->addMessageListener(object, messageListener);
}

void Instance::unregisterMessageListener(MessageListener* messageListener)
{
    messageDispatcher->removeMessageListener(messageListener->object, messageListener);
}

void Instance::registerWeakReference(void* ptr, pd_weak_reference* ref)
{
    weakReferenceLock.enter();
    pdWeakReferences[ptr].add(ref);
    weakReferenceLock.exit();
}

void Instance::unregisterWeakReference(void* ptr, pd_weak_reference const* ref)
{
    weakReferenceLock.enter();

    auto& refs = pdWeakReferences[ptr];

    auto const it = std::ranges::find(refs, ref);

    if (it != refs.end()) {
        refs.erase(it);
    }

    weakReferenceLock.exit();
}

void Instance::clearWeakReferences(void* ptr)
{
    weakReferenceLock.enter();
    for (auto* ref : pdWeakReferences[ptr]) {
        *ref = false;
    }
    pdWeakReferences.erase(ptr);
    weakReferenceLock.exit();
}

void Instance::enqueueFunctionAsync(std::function<void()> const& fn)
{
    functionQueue.enqueue(fn);
}

void Instance::enqueueGuiMessage(Message const& message)
{
    guiMessageQueue.enqueue(message);
    triggerAsyncUpdate();

    // We need to handle pluginmode message on loadbang immediately, to prevent loading Canvas twice
    if (message.selector == "pluginmode" && message.destination == "pd") {
        if (message.list.size() && message.list[0].isFloat() && message.list[0].getFloat() == 0.0f)
            return;
        initialiseIntoPluginmode = true;
    }
}

void Instance::sendDirectMessage(void* object, SmallString const& msg, SmallArray<Atom> const&& list)
{
    lockAudioThread();
    processSend(dmessage(this, object, SmallString(), msg, std::move(list)));
    unlockAudioThread();
}

void Instance::sendDirectMessage(void* object, SmallArray<Atom> const&& list)
{
    lockAudioThread();
    processSend(dmessage(this, object, SmallString(), "list", std::move(list)));
    unlockAudioThread();
}

void Instance::sendDirectMessage(void* object, SmallString const& msg)
{
    lockAudioThread();
    processSend(dmessage(this, object, SmallString(), "symbol", SmallArray<Atom>(1, generateSymbol(msg))));
    unlockAudioThread();
}

void Instance::sendDirectMessage(void* object, float const msg)
{
    lockAudioThread();
    processSend(dmessage(this, object, String(), "float", SmallArray<Atom>(1, msg)));
    unlockAudioThread();
}

void Instance::handleAsyncUpdate()
{
    Message mess;
    while (guiMessageQueue.try_dequeue(mess)) {

        switch (hash(mess.destination)) {
        case hash("pd"):
            receiveSysMessage(mess.selector, mess.list);
            break;
        case hash("__latency_compensation"):
            if (mess.list.size() == 1) {
                if (!mess.list[0].isFloat())
                    return;
                performLatencyCompensationChange(mess.list[0].getFloat());
            }
            break;
        case hash("__param"):
            handleParameterMessage(mess.list);
            break;
        case hash("__to_daw_databuffer"):
            fillDataBuffer(mess.list);
            break;
        case hash("__pd_mcp_debug"):
            handleDebugMessage(mess);
            break;
        default:
            break;
        }
    }
}

void Instance::handleDebugMessage(Message const& message)
{
    auto const reply = [this](var const& response, int const requestId) {
        lockAudioThread();
        sendDebugReplyUnderLock(response, requestId);
        unlockAudioThread();
    };

    if (message.selector != "request" || message.list.size() != 1 || !message.list[0].isSymbol()) {
        reply(makeDebugError(0, "InvalidEnvelope", "Expected request plus one symbol atom"), 0);
        return;
    }

    auto const* encodedBytes = message.list[0].getSymbol()->s_name;
    auto const encodedSize = static_cast<int>(strlen(encodedBytes));
    if (encodedSize > maxEncodedDebugRequestBytes) {
        reply(makeDebugError(0, "PayloadTooLarge", "Encoded request exceeds 64 KiB"), 0);
        return;
    }

    String const encoded = String::fromUTF8(encodedBytes);
    MemoryOutputStream decoded;
    if (!Base64::convertFromBase64(decoded, encoded)
        || Base64::toBase64(decoded.getData(), decoded.getDataSize()) != encoded) {
        reply(makeDebugError(0, "InvalidEnvelope", "Request payload is not canonical base64"), 0);
        return;
    }

    auto const decodedSize = static_cast<int>(decoded.getDataSize());
    if (decodedSize > maxDecodedDebugRequestBytes) {
        reply(makeDebugError(0, "PayloadTooLarge", "Decoded request exceeds 48 KiB"), 0);
        return;
    }

    auto const* decodedBytes = static_cast<char const*>(decoded.getData());
    if (memchr(decodedBytes, 0, static_cast<size_t>(decodedSize)) != nullptr
        || !CharPointer_UTF8::isValidString(decodedBytes, decodedSize)) {
        reply(makeDebugError(0, "InvalidEnvelope", "Decoded request is not valid UTF-8 JSON"), 0);
        return;
    }

    StrictJsonValidator validator(std::string_view(decodedBytes, static_cast<size_t>(decodedSize)));
    if (!validator.parse()) {
        reply(makeDebugError(0, "InvalidEnvelope", "Decoded request must contain strict JSON"), 0);
        return;
    }

    if (!validator.hasObjectRoot()) {
        reply(makeDebugError(0, "InvalidEnvelope", "Decoded request must be a JSON object"), 0);
        return;
    }

    int requestId = 0;
    auto const& requestIdField = validator.getRequestId();
    if (requestIdField.occurrences != 1 || !parseDebugRequestId(requestIdField.value, requestId)) {
        reply(makeDebugError(0, "InvalidRequest", "request_id must be an integer from 1 through 16777215"), 0);
        return;
    }

    auto const& versionField = validator.getVersion();
    if (versionField.occurrences != 1 || versionField.value != "1") {
        reply(makeDebugError(requestId, "UnsupportedProtocolVersion", "Only protocol version 1 is supported"), requestId);
        return;
    }

    String const json = String::fromUTF8(decodedBytes, decodedSize);
    var envelope;
    auto const parseResult = JSON::parse(json, envelope);
    auto* request = envelope.getDynamicObject();
    if (parseResult.failed() || !request) {
        reply(makeDebugError(0, "InvalidEnvelope", "Decoded request must be a JSON object"), 0);
        return;
    }

    auto const operation = request->getProperty("operation");
    if (!operation.isString() || (operation.toString() != "set_generation" && operation.toString() != "send_object")) {
        reply(makeDebugError(requestId, "UnknownOperation", "Unknown debug operation"), requestId);
        return;
    }

    if (operation.toString() == "send_object") {
        if (!hasOnlySendObjectFields(*request)) {
            reply(makeDebugError(requestId, "InvalidRequest", "send_object contains unknown or missing fields"), requestId);
            return;
        }

        auto const generationValue = request->getProperty("generation");
        auto const canvasPathValue = request->getProperty("canvas_path");
        auto const objectOrdinalValue = request->getProperty("object_ordinal");
        auto const selectorValue = request->getProperty("selector");
        auto const atomsValue = request->getProperty("atoms");
        if (!generationValue.isString() || !canvasPathValue.isArray()
            || !(objectOrdinalValue.isInt() || objectOrdinalValue.isInt64())
            || !selectorValue.isString() || !atomsValue.isArray()) {
            reply(makeDebugError(requestId, "InvalidRequest", "send_object fields have invalid types"), requestId);
            return;
        }

        String const generation = generationValue.toString();
        String const selector = selectorValue.toString();
        auto const* canvasPath = canvasPathValue.getArray();
        auto const* atoms = atomsValue.getArray();
        if (generation.isEmpty() || generation.getNumBytesAsUTF8() > 128 || canvasPath == nullptr || canvasPath->size() > 32
            || static_cast<int64>(objectOrdinalValue) < 0 || static_cast<int64>(objectOrdinalValue) > INT_MAX
            || atoms == nullptr || atoms->size() > 256) {
            reply(makeDebugError(requestId, "InvalidRequest", "send_object fields are out of bounds"), requestId);
            return;
        }

        Array<int> path;
        for (auto const& item : *canvasPath) {
            if (!(item.isInt() || item.isInt64()) || static_cast<int64>(item) < 0 || static_cast<int64>(item) > INT_MAX) {
                reply(makeDebugError(requestId, "InvalidRequest", "canvas_path must contain non-negative integers"), requestId);
                return;
            }
            path.add(static_cast<int>(item));
        }

        if (selector.isEmpty() || selector.getNumBytesAsUTF8() > 1024
            || (selector == "bang" && !atoms->isEmpty())
            || (selector == "float" && atoms->size() != 1)
            || (selector == "symbol" && atoms->size() != 1)) {
            reply(makeDebugError(requestId, "InvalidSelector", "Selector is empty, too large, or has invalid arity"), requestId);
            return;
        }

        Array<float> numericAtoms;
        numericAtoms.ensureStorageAllocated(atoms->size());
        for (auto const& atom : *atoms) {
            if (atom.isString()) {
                if (atom.toString().getNumBytesAsUTF8() > 2048) {
                    reply(makeDebugError(requestId, "InvalidAtoms", "String atoms must not exceed 2048 UTF-8 bytes"), requestId);
                    return;
                }
                numericAtoms.add(0.0f);
            } else if (!atom.isDouble() && !atom.isInt() && !atom.isInt64()) {
                reply(makeDebugError(requestId, "InvalidAtoms", "Atoms must be finite numbers or strings"), requestId);
                return;
            } else {
                auto const numericAtom = static_cast<float>(static_cast<double>(atom));
                if (!std::isfinite(numericAtom)) {
                    reply(makeDebugError(requestId, "InvalidAtoms", "Atoms must be finite numbers or strings"), requestId);
                    return;
                }
                numericAtoms.add(numericAtom);
            }
        }

        if ((selector == "float" && atoms->getFirst().isString())
            || (selector == "symbol" && !atoms->getFirst().isString())) {
            reply(makeDebugError(requestId, "InvalidAtoms", "Atoms must match the selector's required type"), requestId);
            return;
        }

        String runtimeError;
        lockAudioThread();
        if (debugGeneration != generation) {
            runtimeError = "StaleGeneration";
        } else if (!debugRoot || !debugRoot->isValid()) {
            runtimeError = "CanvasNotFound";
        } else {
            auto* canvas = debugRoot->getRawUnchecked<t_canvas>();
            if (pd_class(&canvas->gl_obj.ob_pd) != canvas_class) {
                runtimeError = "CanvasNotFound";
            } else {
                for (auto const ordinal : path) {
                    auto* object = canvas->gl_list;
                    for (int index = 0; object && index < ordinal; ++index)
                        object = object->g_next;
                    if (!object) {
                        runtimeError = "CanvasNotFound";
                        break;
                    }
                    if (pd_class(&object->g_pd) != canvas_class) {
                        runtimeError = "CanvasTypeMismatch";
                        break;
                    }
                    canvas = reinterpret_cast<t_canvas*>(object);
                }

                if (runtimeError.isEmpty()) {
                    auto* object = canvas->gl_list;
                    auto const ordinal = static_cast<int>(objectOrdinalValue);
                    for (int index = 0; object && index < ordinal; ++index)
                        object = object->g_next;
                    if (!object)
                        runtimeError = "ObjectNotFound";
                    else if (auto* resolvedObject = pd::Interface::checkObject(&object->g_pd); !resolvedObject || resolvedObject->te_type != T_OBJECT)
                        runtimeError = "ObjectTypeMismatch";
                    else {
                        SmallArray<Atom> resolvedAtoms;
                        resolvedAtoms.reserve(atoms->size());
                        for (int index = 0; index < atoms->size(); ++index) {
                            auto const& atom = atoms->getReference(index);
                            resolvedAtoms.add(atom.isString() ? Atom(generateSymbol(atom.toString())) : Atom(numericAtoms.getUnchecked(index)));
                        }
                        dispatchResolvedMessage(&object->g_pd, SmallString(selector), resolvedAtoms);
                    }
                }
            }
        }
        unlockAudioThread();

        if (runtimeError.isNotEmpty()) {
            auto const message = runtimeError == "StaleGeneration" ? "Generation does not match the active generation" : "Debug target could not be resolved";
            reply(makeDebugError(requestId, runtimeError, message), requestId);
        } else {
            reply(makeDebugInvocationSuccess(requestId), requestId);
        }
        return;
    }

    if (!hasOnlySetGenerationFields(*request)) {
        clearDebugGeneration();
        reply(makeDebugError(requestId, "InvalidRequest", "set_generation contains unknown or missing fields"), requestId);
        return;
    }

    auto const generationValue = request->getProperty("generation");
    auto const rootReceiverValue = request->getProperty("root_receiver");
    if (!generationValue.isString() || !rootReceiverValue.isString()) {
        clearDebugGeneration();
        reply(makeDebugError(requestId, "InvalidRequest", "generation and root_receiver must be strings"), requestId);
        return;
    }

    String const generation = generationValue.toString();
    String const rootReceiver = rootReceiverValue.toString();
    if (generation.isEmpty() || generation.getNumBytesAsUTF8() > maxGenerationBytes
        || rootReceiver.isEmpty() || rootReceiver.getNumBytesAsUTF8() > maxRootReceiverBytes) {
        clearDebugGeneration();
        reply(makeDebugError(requestId, "InvalidRequest", "generation and root_receiver must contain 1 through 128 UTF-8 bytes"), requestId);
        return;
    }

    String runtimeError;
    lockAudioThread();
    debugGeneration.clear();
    debugRoot.reset();

    auto* rootObject = generateSymbol(rootReceiver)->s_thing;
    if (!rootObject) {
        runtimeError = "CanvasNotFound";
    } else if (pd_class(rootObject) != canvas_class) {
        runtimeError = "CanvasTypeMismatch";
    } else {
        auto root = std::make_unique<WeakReference>(rootObject, this);
        if (!root->isValid() || pd_class(&root->getRawUnchecked<t_canvas>()->gl_obj.ob_pd) != canvas_class) {
            runtimeError = "CanvasNotFound";
        } else {
            debugGeneration = generation;
            debugRoot = std::move(root);
        }
    }
    unlockAudioThread();

    if (runtimeError.isNotEmpty()) {
        String const message = runtimeError == "CanvasNotFound" ? "The root receiver is not bound" : "The root receiver is not a canvas";
        reply(makeDebugError(requestId, runtimeError, message), requestId);
        return;
    }

    reply(makeDebugSuccess(requestId), requestId);
}

void Instance::sendDebugReplyUnderLock(var const& response, int const requestId)
{
    String encoded = Base64::toBase64(JSON::toString(response, true));
    if (encoded.getNumBytesAsUTF8() > maxEncodedDebugResponseBytes)
        encoded = Base64::toBase64(JSON::toString(makeDebugError(requestId, "ResponseTooLarge", "Encoded response exceeds 60 KiB"), true));

    sendMessage("__pd_mcp_debug_reply", "response", { generateSymbol(encoded) });
}

void Instance::clearDebugGeneration()
{
    lockAudioThread();
    debugGeneration.clear();
    debugRoot.reset();
    unlockAudioThread();
}

bool Instance::isDebugGenerationActiveUnderLock(String const& generation) const
{
    if (debugGeneration != generation || !debugRoot || !debugRoot->isValid())
        return false;

    return pd_class(&debugRoot->getRawUnchecked<t_canvas>()->gl_obj.ob_pd) == canvas_class;
}

void Instance::KeyHandler::convertJUCEKeyToPd(int& keynum, t_symbol*& keysym)
{
    if (keynum == shiftKey)
        keynum = 0, keysym = pd->generateSymbol("Shift_L");
    else if (keynum == commandKey)
        keynum = 0, keysym = pd->generateSymbol("Meta_L");
    else if (keynum == altKey)
        keynum = 0, keysym = pd->generateSymbol("Alt_L");
    else if (keynum == ctrlKey)
        keynum = 0, keysym = pd->generateSymbol("Control_L");
    else if (keynum == KeyPress::backspaceKey)
        keynum = 8, keysym = pd->generateSymbol("BackSpace");
    else if (keynum == KeyPress::tabKey)
        keynum = 9, keysym = pd->generateSymbol("Tab");
    else if (keynum == KeyPress::returnKey)
        keynum = 10, keysym = pd->generateSymbol("Return");
    else if (keynum == KeyPress::escapeKey)
        keynum = 27, keysym = pd->generateSymbol("Escape");
    else if (keynum == KeyPress::spaceKey)
        keynum = 32, keysym = pd->generateSymbol("Space");
    else if (keynum == KeyPress::deleteKey)
        keynum = 127, keysym = pd->generateSymbol("Delete");
    else if (keynum == KeyPress::upKey)
        keynum = 0, keysym = pd->generateSymbol("Up");
    else if (keynum == KeyPress::downKey)
        keynum = 0, keysym = pd->generateSymbol("Down");
    else if (keynum == KeyPress::leftKey)
        keynum = 0, keysym = pd->generateSymbol("Left");
    else if (keynum == KeyPress::rightKey)
        keynum = 0, keysym = pd->generateSymbol("Right");
    else if (keynum == KeyPress::homeKey)
        keynum = 0, keysym = pd->generateSymbol("Home");
    else if (keynum == KeyPress::endKey)
        keynum = 0, keysym = pd->generateSymbol("End");
    else if (keynum == KeyPress::pageUpKey)
        keynum = 0, keysym = pd->generateSymbol("Prior");
    else if (keynum == KeyPress::pageDownKey)
        keynum = 0, keysym = pd->generateSymbol("Next");
    else if (keynum == KeyPress::F1Key)
        keynum = 0, keysym = pd->generateSymbol("F1");
    else if (keynum == KeyPress::F2Key)
        keynum = 0, keysym = pd->generateSymbol("F2");
    else if (keynum == KeyPress::F3Key)
        keynum = 0, keysym = pd->generateSymbol("F3");
    else if (keynum == KeyPress::F4Key)
        keynum = 0, keysym = pd->generateSymbol("F4");
    else if (keynum == KeyPress::F5Key)
        keynum = 0, keysym = pd->generateSymbol("F5");
    else if (keynum == KeyPress::F6Key)
        keynum = 0, keysym = pd->generateSymbol("F6");
    else if (keynum == KeyPress::F7Key)
        keynum = 0, keysym = pd->generateSymbol("F7");
    else if (keynum == KeyPress::F8Key)
        keynum = 0, keysym = pd->generateSymbol("F8");
    else if (keynum == KeyPress::F9Key)
        keynum = 0, keysym = pd->generateSymbol("F9");
    else if (keynum == KeyPress::F10Key)
        keynum = 0, keysym = pd->generateSymbol("F10");
    else if (keynum == KeyPress::F11Key)
        keynum = 0, keysym = pd->generateSymbol("F11");
    else if (keynum == KeyPress::F12Key)
        keynum = 0, keysym = pd->generateSymbol("F12");
    else if (keynum == KeyPress::numberPad0)
        keynum = 48, keysym = pd->generateSymbol("0");
    else if (keynum == KeyPress::numberPad1)
        keynum = 49, keysym = pd->generateSymbol("1");
    else if (keynum == KeyPress::numberPad2)
        keynum = 50, keysym = pd->generateSymbol("2");
    else if (keynum == KeyPress::numberPad3)
        keynum = 51, keysym = pd->generateSymbol("3");
    else if (keynum == KeyPress::numberPad4)
        keynum = 52, keysym = pd->generateSymbol("4");
    else if (keynum == KeyPress::numberPad5)
        keynum = 53, keysym = pd->generateSymbol("5");
    else if (keynum == KeyPress::numberPad6)
        keynum = 54, keysym = pd->generateSymbol("6");
    else if (keynum == KeyPress::numberPad7)
        keynum = 55, keysym = pd->generateSymbol("7");
    else if (keynum == KeyPress::numberPad8)
        keynum = 56, keysym = pd->generateSymbol("8");
    else if (keynum == KeyPress::numberPad9)
        keynum = 57, keysym = pd->generateSymbol("9");
#if JUCE_MAC || JUCE_WINDOWS
    else if (!ModifierKeys::currentModifiers.isShiftDown() && keynum >= 65 && keynum <= 90) {
        keynum += 32;
    }
#endif
}

void Instance::KeyHandler::shiftKeyChanged(bool const isHeld)
{
    if (isHeld)
        sendKeyPress(KeyPress(shiftKey));
}
void Instance::KeyHandler::commandKeyChanged(bool const isHeld)
{
    if (isHeld)
        sendKeyPress(KeyPress(commandKey));
}
void Instance::KeyHandler::altKeyChanged(bool const isHeld)
{
    if (isHeld)
        sendKeyPress(KeyPress(altKey));
}
void Instance::KeyHandler::ctrlKeyChanged(bool const isHeld)
{
    if (isHeld)
        sendKeyPress(KeyPress(ctrlKey));
}

void Instance::KeyHandler::spaceKeyChanged(bool const isHeld)
{
    if (isHeld)
        sendKeyPress(KeyPress(KeyPress::spaceKey));
}

void Instance::KeyHandler::sendKeyPress(KeyPress const& key)
{
    auto keycode = key.getKeyCode();

#if JUCE_LINUX || JUCE_BSD
    if (keycode == 65505 || keycode == 65506     // Shift
        || keycode == 65507 || keycode == 65508  // Control
        || keycode == 65513 || keycode == 65514  // Alt
        || keycode == 65511 || keycode == 65512) // Meta/Super
    {
        return;
    }
#endif

    String keystring = key.getTextDescription().fromLastOccurrenceOf(" ", false, false);
    if (keystring.startsWith("#"))
        keystring = String::charToString(key.getTextCharacter());
    if (!key.getModifiers().isShiftDown())
        keystring = keystring.toLowerCase();

    pd->enqueueFunctionAsync([this, keycode, keystring]() mutable {
        auto* keysym = pd->generateSymbol(keystring);
        convertJUCEKeyToPd(keycode, keysym);
        pd->sendMessage("#key", "float", { static_cast<float>(keycode) });
        pd->sendMessage("#keyname", "list", { 1.0f, keysym });
    });
    heldKeys.add(key);
}

void Instance::KeyHandler::sendKeyUpMessages()
{
    for (auto key : heldKeys) {
        auto keycode = key.getKeyCode();
        bool keydown = KeyPress::isKeyCurrentlyDown(keycode);
        if (keycode == shiftKey)
            keydown = ModifierKeys::currentModifiers.isShiftDown();
        if (keycode == commandKey)
            keydown = ModifierKeys::currentModifiers.isCommandDown();
        if (keycode == altKey)
            keydown = ModifierKeys::currentModifiers.isAltDown();
        if (keycode == ctrlKey)
            keydown = ModifierKeys::currentModifiers.isCtrlDown();

        if (!keydown) {
            auto keychar = String::charToString(key.getTextCharacter());

            pd->enqueueFunctionAsync([this, keycode, keychar]() mutable {
                auto* keysym = pd->generateSymbol(keychar);
                convertJUCEKeyToPd(keycode, keysym);
                pd->sendMessage("#keyup", "float", { static_cast<float>(keycode) });
                pd->sendMessage("#keyname", "list", { 0.0f, keysym });
            });
            heldKeys.remove_one(key);
        }
    }
}

void Instance::sendMessagesFromQueue()
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));

    std::function<void()> callback;
    while (functionQueue.try_dequeue(callback)) {
        callback();
    }
}

Patch::Ptr Instance::openPatch(File const& toOpen)
{
    String const dirname = toOpen.getParentDirectory().getFullPathName().replace("\\", "/");
    auto const* dir = dirname.toRawUTF8();

    String const filename = toOpen.getFileName();
    auto const* file = filename.toRawUTF8();

    setThis();

    auto* cnv = pd::Interface::createCanvas(file, dir);

    return new Patch(pd::WeakReference(cnv, this), this, true, toOpen);
}

void Instance::setThis() const
{
    libpd_set_instance(static_cast<t_pdinstance*>(instance));
}

t_symbol* Instance::generateSymbol(char const* symbol) const
{
    setThis();
    return gensym(symbol);
}

t_symbol* Instance::generateSymbol(String const& symbol) const
{
    return generateSymbol(symbol.toRawUTF8());
}

t_symbol* Instance::generateSymbol(SmallString const& symbol) const
{
    return generateSymbol(symbol.data());
}

void Instance::logMessage(String const& message)
{
    consoleMessageHandler->logMessage(nullptr, message);
}

void Instance::logError(String const& error)
{
    consoleMessageHandler->logError(nullptr, error);
}

void Instance::logWarning(String const& warning)
{
    consoleMessageHandler->logWarning(nullptr, warning);
}

std::deque<std::tuple<void*, String, int, int, int>>& Instance::getConsoleMessages() const
{
    return consoleMessageHandler->consoleMessages;
}

std::deque<std::tuple<void*, String, int, int, int>>& Instance::getConsoleHistory() const
{
    return consoleMessageHandler->consoleHistory;
}

void Instance::createPanel(int const type, char const* snd, char const* location, char const* callbackName, int openMode)
{
#if ENABLE_TESTING
    return; // Don't open file dialogs when running tests, that's annoying
#endif

    auto* obj = generateSymbol(snd)->s_thing;

    auto defaultFile = File(location);
    if (!defaultFile.exists()) {
        defaultFile = SettingsFile::getInstance()->getLastBrowserPathForId("OpenPanel");
        if (!defaultFile.exists())
            defaultFile = ProjectInfo::appDataDir;
    }

    if (type) {
        MessageManager::callAsync(
            [this, obj = generateSymbol(snd)->s_thing, defaultFile, openMode, callback = SmallString(callbackName)]() mutable {
                FileBrowserComponent::FileChooserFlags folderChooserFlags;

                if (openMode <= 0) {
                    folderChooserFlags = static_cast<FileBrowserComponent::FileChooserFlags>(FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles);
                } else if (openMode == 1) {
                    folderChooserFlags = static_cast<FileBrowserComponent::FileChooserFlags>(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories);
                } else {
                    folderChooserFlags = static_cast<FileBrowserComponent::FileChooserFlags>(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories | FileBrowserComponent::canSelectFiles | FileBrowserComponent::canSelectMultipleItems);
                }

                static std::unique_ptr<FileChooser> openChooser;
                openChooser = std::make_unique<FileChooser>("Open...", defaultFile, "", SettingsFile::getInstance()->wantsNativeDialog());

                openChooser->launchAsync(folderChooserFlags, [this, obj, callback](FileChooser const& fileChooser) {
                    auto const files = fileChooser.getResults();
                    if (files.isEmpty())
                        return;

#if JUCE_IOS
                    // Create input streams to access the security scoped resource
                    SmallArray<std::unique_ptr<InputStream>> scopedAccessStreams;
                    for (auto& url : fileChooser.getURLResults()) {
                        scopedAccessStreams.emplace_back(url.createInputStream(URL::InputStreamOptions(URL::ParameterHandling::inAddress)));
                    }
#endif
                    auto const parentDirectory = files.getFirst().getParentDirectory();
                    SettingsFile::getInstance()->setLastBrowserPathForId("OpenPanel", parentDirectory);

                    lockAudioThread();

                    SmallArray<t_atom> atoms(files.size());

                    for (int i = 0; i < atoms.size(); i++) {
                        auto pathname = files[i].getFullPathName();
#if JUCE_WINDOWS
                        pathname = pathname.replaceCharacter('\\', '/');
#endif
                        libpd_set_symbol(atoms.data() + i, pathname.toRawUTF8());
                    }

                    pd_typedmess(obj, generateSymbol(callback), atoms.size(), atoms.data());

                    unlockAudioThread();
                    openChooser.reset(nullptr);
                });
            });
    } else {
        MessageManager::callAsync(
            [this, obj, defaultFile, callback = SmallString(callbackName)]() mutable {

#if JUCE_IOS
                Component* dialogParent = dynamic_cast<AudioProcessor*>(this)->getActiveEditor();
#else
                Component* dialogParent = nullptr;
#endif
                if (defaultFile.exists()) {
                    SettingsFile::getInstance()->setLastBrowserPathForId("SavePanel", defaultFile);
                }

                Dialogs::showSaveDialog([this, obj, callback](URL const& result) {
                    auto pathname = result.getLocalFile().getFullPathName();
#if JUCE_WINDOWS
                    pathname = pathname.replaceCharacter('\\', '/');
#endif

                    auto const* path = pathname.toRawUTF8();

                    t_atom argv;
                    libpd_set_symbol(&argv, path);

                    lockAudioThread();
                    pd_typedmess(obj, generateSymbol(callback), 1, &argv);
                    unlockAudioThread();
                },
                    "", "SavePanel", dialogParent);
            });
    }
}

bool Instance::loadLibrary(String const& libraryToLoad)
{
    return sys_load_lib(nullptr, libraryToLoad.toRawUTF8());
}

void Instance::lockAudioThread()
{
    setThis();
    sys_lock();
}

void Instance::unlockAudioThread()
{
    sys_unlock();
}

void Instance::updateObjectImplementations()
{
    objectImplementations->updateObjectImplementations();
}

void Instance::clearObjectImplementationsForPatch(pd::Patch const* p)
{
    if (auto patch = p->getPointer()) {
        objectImplementations->clearObjectImplementationsForPatch(patch.get());
    }
}

void Instance::registerLuaClass(char const* className)
{
    luaClasses.insert(hash(className));
}

bool Instance::isLuaClass(hash32 const objectNameHash)
{
    return luaClasses.contains(objectNameHash);
}

} // namespace pd
