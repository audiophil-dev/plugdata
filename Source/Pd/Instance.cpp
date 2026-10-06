/*
 // Copyright (c) 2015-2022 Pierre Guillot and Timothy Schoen
 // For information on usage and redistribution, and for a DISCLAIMER OF ALL
 // WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>

#include "Utility/Config.h"
#include "Utility/Fonts.h"
#include "Utility/CachedStringWidth.h"
#include "Dialogs/Dialogs.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "Instance.h"
#include "Object.h"
#include "Connection.h"
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
#if ENABLE_TESTING
void (*canvasEditInterleaveHookForTesting)(void*) = nullptr;
void* canvasEditInterleaveContextForTesting = nullptr;
void (*canvasEditFailureHookForTesting)(void*) = nullptr;
void* canvasEditFailureContextForTesting = nullptr;
void (*canvasEditGuiMismatchHookForTesting)(void*) = nullptr;
void* canvasEditGuiMismatchContextForTesting = nullptr;
void (*exportWriteFailureHookForTesting)(void*) = nullptr;
void* exportWriteFailureContextForTesting = nullptr;
#endif
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

String consoleSeverityToString(int const severity)
{
    switch (severity) {
    case 1:
        return "warning";
    case 2:
        return "error";
    default:
        return "message";
    }
}

var makeConsoleSnapshot(int const requestId, Array<var> const& entries, bool const truncated, int64 const cursor)
{
    auto* data = new DynamicObject();
    data->setProperty("entries", var(entries));
    data->setProperty("truncated", truncated);
    data->setProperty("cursor", cursor);

    auto* response = new DynamicObject();
    response->setProperty("version", debugProtocolVersion);
    response->setProperty("request_id", requestId);
    response->setProperty("ok", true);
    response->setProperty("data", var(data));
    return var(response);
}

var makeClearSuccess(int const requestId)
{
    auto* data = new DynamicObject();
    data->setProperty("status", "cleared");

    auto* response = new DynamicObject();
    response->setProperty("version", debugProtocolVersion);
    response->setProperty("request_id", requestId);
    response->setProperty("ok", true);
    response->setProperty("data", var(data));
    return var(response);
}

var makeExportSuccess(int const requestId, bool const clipped)
{
    auto* data = new DynamicObject();
    data->setProperty("status", "saved");
    data->setProperty("clipped", clipped);

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

bool hasOnlyGetConsoleFields(DynamicObject const& request)
{
    auto const& properties = request.getProperties();
    if (properties.size() != 5 && properties.size() != 6)
        return false;

    bool sawVersion = false;
    bool sawRequestId = false;
    bool sawOperation = false;
    bool sawIncludeHistory = false;
    bool sawMaxEntries = false;
    bool sawSinceId = false;

    for (auto const& [name, value] : properties) {
        ignoreUnused(value);
        auto const propertyName = name.toString();
        if (propertyName == "version")
            sawVersion = true;
        else if (propertyName == "request_id")
            sawRequestId = true;
        else if (propertyName == "operation")
            sawOperation = true;
        else if (propertyName == "include_history")
            sawIncludeHistory = true;
        else if (propertyName == "max_entries")
            sawMaxEntries = true;
        else if (propertyName == "since_id")
            sawSinceId = true;
        else
            return false;
    }

    if (!(sawVersion && sawRequestId && sawOperation && sawIncludeHistory && sawMaxEntries))
        return false;

    return (properties.size() == 6) == sawSinceId;
}

bool hasOnlyClearConsoleFields(DynamicObject const& request)
{
    auto const& properties = request.getProperties();
    if (properties.size() != 3)
        return false;

    for (auto const& [name, value] : properties) {
        ignoreUnused(value);
        auto const propertyName = name.toString();
        if (propertyName != "version" && propertyName != "request_id" && propertyName != "operation")
            return false;
    }
    return true;
}

bool hasFields(DynamicObject const& object, std::initializer_list<char const*> required, std::initializer_list<char const*> optional = {})
{
    auto const& properties = object.getProperties();
    if (properties.size() < static_cast<int>(required.size()) || properties.size() > static_cast<int>(required.size() + optional.size()))
        return false;
    for (auto const* field : required)
        if (!object.hasProperty(field))
            return false;
    for (auto const& [name, value] : properties) {
        ignoreUnused(value);
        bool known = false;
        for (auto const* field : required)
            known |= name.toString() == field;
        for (auto const* field : optional)
            known |= name.toString() == field;
        if (!known)
            return false;
    }
    return true;
}

bool hasOnlyExportCanvasFields(DynamicObject const& request)
{
    return hasFields(request, { "version", "request_id", "operation", "generation", "path" }, { "canvas_path", "scale" });
}

bool boundedString(var const& value, int const maxBytes)
{
    return value.isString() && value.toString().isNotEmpty() && value.toString().getNumBytesAsUTF8() <= maxBytes;
}

var makeCanvasPage(int requestId, String const& revision, Array<var> const& records, var const& nextCursor);

bool nonnegativeOrdinal(var const& value)
{
    return (value.isInt() || value.isInt64()) && static_cast<int64>(value) >= 0 && static_cast<int64>(value) <= INT_MAX;
}

bool boundedCoordinate(var const& value)
{
    return (value.isInt() || value.isInt64()) && static_cast<int64>(value) >= -1000000 && static_cast<int64>(value) <= 1000000;
}

bool batchTarget(var const& value, StringArray const& refs)
{
    if (nonnegativeOrdinal(value))
        return true;
    auto* target = value.getDynamicObject();
    return target && hasFields(*target, { "ref" }) && boundedString(target->getProperty("ref"), 128)
        && refs.contains(target->getProperty("ref").toString());
}

bool canvasPathValid(var const& value, StringArray const& subpatchRefs, bool const allowRefs)
{
    auto const* path = value.getArray();
    if (!path || path->size() > 32)
        return false;
    for (auto const& component : *path) {
        if (nonnegativeOrdinal(component))
            continue;
        if (!allowRefs || !batchTarget(component, subpatchRefs))
            return false;
    }
    return true;
}

bool canvasRequestValid(DynamicObject const& request, bool const edit)
{
    auto const generation = request.getProperty("generation");
    if (!boundedString(generation, maxGenerationBytes))
        return false;

    if (!edit) {
        if (!hasFields(request, { "version", "request_id", "operation", "generation", "canvas_path" }, { "cursor", "expected_revision" })
            || !canvasPathValid(request.getProperty("canvas_path"), {}, false)
            || (request.hasProperty("cursor") && !request.hasProperty("expected_revision")))
            return false;
        for (auto const* field : { "cursor", "expected_revision" })
            if (request.hasProperty(field) && !boundedString(request.getProperty(field), 128))
                return false;
        return true;
    }

    if (!hasFields(request, { "version", "request_id", "operation", "generation", "expected_revision", "edits" })
        || !boundedString(request.getProperty("expected_revision"), 128))
        return false;
    auto const editsValue = request.getProperty("edits");
    auto const* edits = editsValue.getArray();
    if (!edits || edits->isEmpty() || edits->size() > 100)
        return false;

    StringArray createdRefs, liveRefs, subpatchRefs;
    for (auto const& entry : *edits) {
        auto* editObject = entry.getDynamicObject();
        if (!editObject || !boundedString(editObject->getProperty("op"), 16)
            || !canvasPathValid(editObject->getProperty("canvas_path"), subpatchRefs, true))
            return false;
        String const op = editObject->getProperty("op").toString();
        if (op == "add") {
            if (!hasFields(*editObject, { "op", "canvas_path", "ref", "type", "x", "y" }, { "text", "name" })
                || !boundedString(editObject->getProperty("ref"), 128)
                || createdRefs.contains(editObject->getProperty("ref").toString())
                || !boundedCoordinate(editObject->getProperty("x")) || !boundedCoordinate(editObject->getProperty("y")))
                return false;
            String const type = editObject->getProperty("type").toString();
            if (type == "subpatch") {
                if (editObject->hasProperty("text") || !boundedString(editObject->getProperty("name"), 128))
                    return false;
                subpatchRefs.add(editObject->getProperty("ref").toString());
            } else if ((type != "object" && type != "comment") || editObject->hasProperty("name")
                || !boundedString(editObject->getProperty("text"), 2048)) {
                return false;
            }
            createdRefs.add(editObject->getProperty("ref").toString());
            liveRefs.add(editObject->getProperty("ref").toString());
        } else if (op == "move") {
            if (!hasFields(*editObject, { "op", "canvas_path", "target", "x", "y" })
                || !batchTarget(editObject->getProperty("target"), liveRefs)
                || !boundedCoordinate(editObject->getProperty("x")) || !boundedCoordinate(editObject->getProperty("y")))
                return false;
        } else if (op == "remove") {
            if (!hasFields(*editObject, { "op", "canvas_path", "target" }) || !batchTarget(editObject->getProperty("target"), liveRefs))
                return false;
            auto const targetValue = editObject->getProperty("target");
            auto* target = targetValue.getDynamicObject();
            if (target) {
                String const ref = target->getProperty("ref").toString();
                liveRefs.removeString(ref);
                subpatchRefs.removeString(ref);
            }
        } else if (op == "connect" || op == "disconnect") {
            if (!hasFields(*editObject, { "op", "canvas_path", "source", "outlet", "target", "inlet" })
                || !batchTarget(editObject->getProperty("source"), liveRefs)
                || !batchTarget(editObject->getProperty("target"), liveRefs)
                || !nonnegativeOrdinal(editObject->getProperty("outlet")) || !nonnegativeOrdinal(editObject->getProperty("inlet")))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

void appendCanvasRecords(t_canvas* canvas, Array<var> const& path, Array<var>& records)
{
    auto* canvasRecord = new DynamicObject();
    canvasRecord->setProperty("kind", "canvas");
    canvasRecord->setProperty("canvas_path", var(path));
    canvasRecord->setProperty("name", String::fromUTF8(canvas->gl_name->s_name));
    records.add(var(canvasRecord));

    int ordinal = 0;
    for (auto* object = canvas->gl_list; object; object = object->g_next, ++ordinal) {
        auto* record = new DynamicObject();
        record->setProperty("kind", "object");
        record->setProperty("canvas_path", var(path));
        record->setProperty("ordinal", ordinal);
        if (pd_class(&object->g_pd) == canvas_class) {
            record->setProperty("type", "subpatch");
            record->setProperty("name", String::fromUTF8(reinterpret_cast<t_canvas*>(object)->gl_name->s_name));
        } else if (auto* text = Interface::checkObject(object)) {
            record->setProperty("type", text->te_type == T_TEXT ? "comment" : text->te_type == T_MESSAGE ? "message" : text->te_type == T_ATOM ? "atom" : "object");
            record->setProperty("text", Interface::getObjectText(text).toString());
        } else {
            record->setProperty("type", String::fromUTF8(class_getname(pd_class(&object->g_pd))));
            auto* definition = binbuf_new();
            gobj_save(object, definition);
            char* bytes = nullptr;
            int size = 0;
            binbuf_gettext(definition, &bytes, &size);
            record->setProperty("definition", String::fromUTF8(bytes, size));
            freebytes(bytes, static_cast<size_t>(size));
            binbuf_free(definition);
        }
        if (auto* text = Interface::checkObject(object)) {
            record->setProperty("x", text->te_xpix);
            record->setProperty("y", text->te_ypix);
        } else if (pd_class(&object->g_pd) == scalar_class) {
            t_float x = 0, y = 0;
            auto* scalar = reinterpret_cast<t_scalar*>(object);
            scalar_getbasexy(scalar, &x, &y);
            record->setProperty("template", String::fromUTF8(scalar->sc_template->s_name));
            record->setProperty("x", static_cast<double>(x));
            record->setProperty("y", static_cast<double>(y));
            record->setProperty("coordinate_space", "data");
        } else {
            record->setProperty("position_available", false);
        }
        records.add(var(record));
    }

    t_linetraverser traversal;
    linetraverser_start(&traversal, canvas);
    while (linetraverser_next_nosize(&traversal)) {
        auto* record = new DynamicObject();
        record->setProperty("kind", "connection");
        record->setProperty("canvas_path", var(path));
        record->setProperty("source", canvas_getindex(canvas, &traversal.tr_ob->ob_g));
        record->setProperty("outlet", traversal.tr_outno);
        record->setProperty("target", canvas_getindex(canvas, &traversal.tr_ob2->ob_g));
        record->setProperty("inlet", traversal.tr_inno);
        if (traversal.outconnect_path_info != gensym("empty"))
            record->setProperty("path_info", String::fromUTF8(traversal.outconnect_path_info->s_name));
        records.add(var(record));
    }

    ordinal = 0;
    for (auto* object = canvas->gl_list; object; object = object->g_next, ++ordinal)
        if (pd_class(&object->g_pd) == canvas_class) {
            auto childPath = path;
            childPath.add(ordinal);
            appendCanvasRecords(reinterpret_cast<t_canvas*>(object), childPath, records);
    }
}

bool canvasIsInRoot(t_canvas* root, t_canvas* target)
{
    if (root == target)
        return true;
    for (auto* object = root->gl_list; object; object = object->g_next)
        if (pd_class(&object->g_pd) == canvas_class
            && canvasIsInRoot(reinterpret_cast<t_canvas*>(object), target))
            return true;
    return false;
}

bool canvasGuiMatchesPd(Canvas* canvas)
{
    auto* native = canvas->patch.getRawPointer();
    if (!native || canvas->objects.size() != [&] {
            int count = 0;
            for (auto* object = native->gl_list; object; object = object->g_next)
                ++count;
            return count;
        }())
        return false;

    int ordinal = 0;
    for (auto* object = native->gl_list; object; object = object->g_next, ++ordinal) {
        auto* shown = canvas->objects[ordinal];
        if (!shown || shown->getPointer() != object)
            return false;
        if (canvas->isGraph && !shown->isVisible())
            continue;
        if (Interface::checkObject(object)) {
            int x = 0, y = 0, width = 0, height = 0;
            Interface::getObjectBounds(native, object, &x, &y, &width, &height);
            if (shown->getObjectBounds().getPosition() != Point<int>(x, y))
                return false;
        }
    }

    if (canvas->isGraph)
        return canvas->connections.size() == 0;

    int connectionCount = 0;
    t_linetraverser traversal;
    linetraverser_start(&traversal, native);
    while (auto* nativeConnection = linetraverser_next_nosize(&traversal)) {
        bool found = false;
        for (auto* shown : canvas->connections)
            if (shown && shown->getPointer() == nativeConnection
                && shown->outobj && shown->outobj->getPointer() == &traversal.tr_ob->ob_g
                && shown->inobj && shown->inobj->getPointer() == &traversal.tr_ob2->ob_g
                && shown->outIdx == traversal.tr_outno && shown->inIdx == traversal.tr_inno) {
                found = true;
                break;
            }
        if (!found)
            return false;
        ++connectionCount;
    }
    return canvas->connections.size() == connectionCount;
}

struct ProjectedCanvasConnection {
    int source;
    int outlet;
    int target;
    int inlet;
};

struct ProjectedCanvasObject {
    t_object* pdObject = nullptr;
    int canvasIndex = -1;
    int childCanvasIndex = -1;
    bool isSubpatch = false;
    bool preState = true;
    bool active = true;
    bool portsKnown = true;
    int inletCount = 0;
    int outletCount = 0;
    int parentPortIndex = -1;
    std::vector<bool> signalInlets;
    std::vector<bool> signalOutlets;
};

struct ProjectedCanvas {
    t_canvas* pdCanvas = nullptr;
    bool active = true;
    std::vector<int> preStateObjects;
    std::vector<int> projectedObjects;
    std::vector<ProjectedCanvasConnection> connections;
};

bool preflightCanvasEdits(t_canvas* root, Array<var> const& edits, String& error, std::vector<t_canvas*>& removedSubpatches)
{
    std::vector<ProjectedCanvas> canvases;
    std::vector<ProjectedCanvasObject> objects;
    std::vector<int> movedPortParents;
    StringArray refs;
    std::vector<int> refObjects;
    removedSubpatches.clear();

    auto addCanvas = [&](auto&& self, t_canvas* canvas) -> int {
        int const canvasIndex = static_cast<int>(canvases.size());
        canvases.emplace_back();
        canvases[static_cast<size_t>(canvasIndex)].pdCanvas = canvas;

        for (auto* item = canvas->gl_list; item; item = item->g_next) {
            bool const isSubpatch = pd_class(&item->g_pd) == canvas_class;
            auto* pdObject = isSubpatch
                ? &reinterpret_cast<t_canvas*>(item)->gl_obj
                : Interface::checkObject(item);
            ProjectedCanvasObject projectedObject;
            projectedObject.pdObject = pdObject;
            projectedObject.canvasIndex = canvasIndex;
            projectedObject.isSubpatch = isSubpatch;
            if (pdObject) {
                projectedObject.inletCount = Interface::numInlets(pdObject);
                projectedObject.outletCount = Interface::numOutlets(pdObject);
                for (int inlet = 0; inlet < projectedObject.inletCount; ++inlet)
                    projectedObject.signalInlets.push_back(Interface::isSignalInlet(pdObject, inlet));
                for (int outlet = 0; outlet < projectedObject.outletCount; ++outlet)
                    projectedObject.signalOutlets.push_back(Interface::isSignalOutlet(pdObject, outlet));
            }
            int const objectIndex = static_cast<int>(objects.size());
            objects.push_back(projectedObject);
            auto& projectedCanvas = canvases[static_cast<size_t>(canvasIndex)];
            projectedCanvas.preStateObjects.push_back(objectIndex);
            projectedCanvas.projectedObjects.push_back(objectIndex);

            if (isSubpatch) {
                int const childIndex = self(self, reinterpret_cast<t_canvas*>(item));
                objects[static_cast<size_t>(objectIndex)].childCanvasIndex = childIndex;
            }
        }
        return canvasIndex;
    };

    addCanvas(addCanvas, root);

    for (auto const& owner : objects) {
        if (owner.childCanvasIndex < 0)
            continue;
        auto const& child = canvases[static_cast<size_t>(owner.childCanvasIndex)];
        for (auto* portClass : { vinlet_class, voutlet_class }) {
            std::vector<int> ports;
            for (auto const objectIndex : child.preStateObjects) {
                auto* port = objects[static_cast<size_t>(objectIndex)].pdObject;
                if (port && pd_class(&port->ob_pd) == portClass)
                    ports.push_back(objectIndex);
            }
            std::sort(ports.begin(), ports.end(), [&](int left, int right) {
                int const leftX = objects[static_cast<size_t>(left)].pdObject->te_xpix;
                int const rightX = objects[static_cast<size_t>(right)].pdObject->te_xpix;
                return leftX == rightX ? left > right : leftX < rightX;
            });
            for (int index = 0; index < static_cast<int>(ports.size()); ++index)
                objects[static_cast<size_t>(ports[static_cast<size_t>(index)])].parentPortIndex = index;
        }
    }

    for (int canvasIndex = 0; canvasIndex < static_cast<int>(canvases.size()); ++canvasIndex) {
        auto* canvas = canvases[static_cast<size_t>(canvasIndex)].pdCanvas;
        t_linetraverser traversal;
        linetraverser_start(&traversal, canvas);
        while (linetraverser_next_nosize(&traversal)) {
            int const sourceOrdinal = canvas_getindex(canvas, &traversal.tr_ob->ob_g);
            int const targetOrdinal = canvas_getindex(canvas, &traversal.tr_ob2->ob_g);
            auto const& preState = canvases[static_cast<size_t>(canvasIndex)].preStateObjects;
            if (sourceOrdinal < 0 || targetOrdinal < 0
                || sourceOrdinal >= static_cast<int>(preState.size()) || targetOrdinal >= static_cast<int>(preState.size())) {
                error = "InvalidConnection";
                return false;
            }
            canvases[static_cast<size_t>(canvasIndex)].connections.push_back({
                preState[static_cast<size_t>(sourceOrdinal)], traversal.tr_outno,
                preState[static_cast<size_t>(targetOrdinal)], traversal.tr_inno });
        }
    }

    auto findRef = [&](var const& value) {
        auto* ref = value.getDynamicObject();
        return ref ? refs.indexOf(ref->getProperty("ref").toString()) : -1;
    };

    auto resolveCanvas = [&](var const& pathValue) {
        auto const* path = pathValue.getArray();
        int canvasIndex = 0;
        for (auto const& component : *path) {
            int objectIndex = -1;
            if (nonnegativeOrdinal(component)) {
                auto const ordinal = static_cast<int>(component);
                auto const& preState = canvases[static_cast<size_t>(canvasIndex)].preStateObjects;
                if (ordinal >= static_cast<int>(preState.size())) {
                    error = "CanvasNotFound";
                    return -1;
                }
                objectIndex = preState[static_cast<size_t>(ordinal)];
            } else {
                int const refIndex = findRef(component);
                if (refIndex >= 0)
                    objectIndex = refObjects[static_cast<size_t>(refIndex)];
            }

            if (objectIndex < 0 || !objects[static_cast<size_t>(objectIndex)].active
                || objects[static_cast<size_t>(objectIndex)].canvasIndex != canvasIndex) {
                error = "CanvasNotFound";
                return -1;
            }
            if (!objects[static_cast<size_t>(objectIndex)].isSubpatch) {
                error = "CanvasTypeMismatch";
                return -1;
            }
            if (objects[static_cast<size_t>(objectIndex)].childCanvasIndex < 0) {
                error = "CanvasNotFound";
                return -1;
            }
            canvasIndex = objects[static_cast<size_t>(objectIndex)].childCanvasIndex;
            if (!canvases[static_cast<size_t>(canvasIndex)].active) {
                error = "CanvasNotFound";
                return -1;
            }
        }
        return canvasIndex;
    };

    auto resolveTarget = [&](var const& value, int canvasIndex, bool allowOtherCanvas) {
        int objectIndex = -1;
        if (nonnegativeOrdinal(value)) {
            int const ordinal = static_cast<int>(value);
            auto const& preState = canvases[static_cast<size_t>(canvasIndex)].preStateObjects;
            if (ordinal < static_cast<int>(preState.size()))
                objectIndex = preState[static_cast<size_t>(ordinal)];
        } else {
            int const refIndex = findRef(value);
            if (refIndex >= 0)
                objectIndex = refObjects[static_cast<size_t>(refIndex)];
        }
        if (objectIndex < 0 || !objects[static_cast<size_t>(objectIndex)].active
            || (!allowOtherCanvas && objects[static_cast<size_t>(objectIndex)].canvasIndex != canvasIndex)) {
            error = "ObjectNotFound";
            return -1;
        }
        return objectIndex;
    };

    auto deactivateCanvas = [&](auto&& self, int canvasIndex) -> void {
        auto& canvas = canvases[static_cast<size_t>(canvasIndex)];
        if (!canvas.active)
            return;
        canvas.active = false;
        for (auto const objectIndex : canvas.projectedObjects) {
            auto& object = objects[static_cast<size_t>(objectIndex)];
            if (!object.active)
                continue;
            object.active = false;
            if (object.childCanvasIndex >= 0)
                self(self, object.childCanvasIndex);
        }
        canvas.projectedObjects.clear();
        canvas.connections.clear();
    };

    auto collectSubpatchCanvases = [&](int const firstCanvasIndex) {
        std::vector<int> pendingCanvasIndices { firstCanvasIndex };
        while (!pendingCanvasIndices.empty()) {
            int const canvasIndex = pendingCanvasIndices.back();
            pendingCanvasIndices.pop_back();
            auto const& canvas = canvases[static_cast<size_t>(canvasIndex)];
            if (canvas.pdCanvas && std::find(removedSubpatches.begin(), removedSubpatches.end(), canvas.pdCanvas) == removedSubpatches.end())
                removedSubpatches.push_back(canvas.pdCanvas);
            for (auto const objectIndex : canvas.preStateObjects) {
                auto const& object = objects[static_cast<size_t>(objectIndex)];
                if (object.isSubpatch && object.childCanvasIndex >= 0)
                    pendingCanvasIndices.push_back(object.childCanvasIndex);
            }
        }
    };

    auto validatePorts = [&](int sourceIndex, int outlet, int targetIndex, int inlet, bool checkSignalCompatibility) {
        auto const& source = objects[static_cast<size_t>(sourceIndex)];
        auto const& target = objects[static_cast<size_t>(targetIndex)];
        if ((source.portsKnown && outlet >= source.outletCount)
            || (target.portsKnown && inlet >= target.inletCount)) {
            error = "InvalidPort";
            return false;
        }
        if (checkSignalCompatibility && source.portsKnown && target.portsKnown
            && source.signalOutlets[static_cast<size_t>(outlet)] && !target.signalInlets[static_cast<size_t>(inlet)]) {
            error = "InvalidConnection";
            return false;
        }
        return true;
    };

    auto hasConnection = [](std::vector<ProjectedCanvasConnection> const& connections, int source, int outlet, int target, int inlet) {
        return std::any_of(connections.begin(), connections.end(), [&](ProjectedCanvasConnection const& connection) {
            return connection.source == source && connection.outlet == outlet
                && connection.target == target && connection.inlet == inlet;
        });
    };

    for (auto const& editValue : edits) {
        auto* edit = editValue.getDynamicObject();
        int const canvasIndex = resolveCanvas(edit->getProperty("canvas_path"));
        if (canvasIndex < 0)
            return false;

        String const op = edit->getProperty("op").toString();
        if (op == "add") {
            String const ref = edit->getProperty("ref").toString();
            String const type = edit->getProperty("type").toString();
            if (type == "object" && canvasIndex != 0) {
                for (auto& object : objects)
                    if (object.childCanvasIndex == canvasIndex)
                        object.portsKnown = false;
            }
            if (type == "object") {
                auto* words = binbuf_new();
                auto const text = edit->getProperty("text").toString();
                binbuf_text(words, text.toRawUTF8(), text.getNumBytesAsUTF8());
                bool const createsSubpatch = binbuf_getnatom(words) > 0
                    && binbuf_getvec(words)[0].a_type == A_SYMBOL
                    && atom_getsymbol(binbuf_getvec(words)) == gensym("pd");
                binbuf_free(words);
                if (createsSubpatch) {
                    error = "InvalidRequest";
                    return false;
                }
            }
            bool const isSubpatch = type == "subpatch";
            ProjectedCanvasObject object;
            object.canvasIndex = canvasIndex;
            object.isSubpatch = isSubpatch;
            object.preState = false;
            object.portsKnown = type == "comment";
            int const objectIndex = static_cast<int>(objects.size());
            objects.push_back(object);
            canvases[static_cast<size_t>(canvasIndex)].projectedObjects.push_back(objectIndex);
            refs.add(ref);
            refObjects.push_back(objectIndex);
            if (isSubpatch) {
                int const childIndex = static_cast<int>(canvases.size());
                canvases.emplace_back();
                objects[static_cast<size_t>(objectIndex)].childCanvasIndex = childIndex;
            }
            continue;
        }

        if (op == "move") {
            int const objectIndex = resolveTarget(edit->getProperty("target"), canvasIndex, false);
            if (objectIndex < 0)
                return false;
            auto const& object = objects[static_cast<size_t>(objectIndex)];
            if (object.isSubpatch || (object.preState && !object.pdObject)) {
                error = "ObjectTypeMismatch";
                return false;
            }
            if (object.pdObject && (pd_class(&object.pdObject->ob_pd) == vinlet_class
                    || pd_class(&object.pdObject->ob_pd) == voutlet_class)) {
                for (int parentIndex = 0; parentIndex < static_cast<int>(objects.size()); ++parentIndex)
                    if (objects[static_cast<size_t>(parentIndex)].active
                        && objects[static_cast<size_t>(parentIndex)].childCanvasIndex == canvasIndex
                        && std::find(movedPortParents.begin(), movedPortParents.end(), parentIndex) == movedPortParents.end())
                        movedPortParents.push_back(parentIndex);
            }
            continue;
        }

        if (op == "remove") {
            int const objectIndex = resolveTarget(edit->getProperty("target"), canvasIndex, false);
            if (objectIndex < 0)
                return false;
            auto& canvas = canvases[static_cast<size_t>(canvasIndex)];
            auto& object = objects[static_cast<size_t>(objectIndex)];
            if (object.parentPortIndex >= 0 && std::any_of(objects.begin(), objects.end(), [&](auto const& parent) {
                    return parent.active && parent.childCanvasIndex == canvasIndex
                        && std::find(movedPortParents.begin(), movedPortParents.end(), &parent - objects.data()) != movedPortParents.end();
                })) {
                error = "InvalidConnection";
                return false;
            }
            if (object.pdObject && object.parentPortIndex >= 0) {
                bool const inletPort = pd_class(&object.pdObject->ob_pd) == vinlet_class;
                for (auto& parent : objects) {
                    if (!parent.active || parent.childCanvasIndex != canvasIndex)
                        continue;
                    auto& parentConnections = canvases[static_cast<size_t>(parent.canvasIndex)].connections;
                    int const parentIndex = static_cast<int>(&parent - objects.data());
                    parentConnections.erase(std::remove_if(parentConnections.begin(), parentConnections.end(), [&](auto const& connection) {
                        return inletPort ? connection.target == parentIndex && connection.inlet == object.parentPortIndex
                                         : connection.source == parentIndex && connection.outlet == object.parentPortIndex;
                    }), parentConnections.end());
                    for (auto& connection : parentConnections) {
                        if (inletPort && connection.target == parentIndex && connection.inlet > object.parentPortIndex)
                            --connection.inlet;
                        if (!inletPort && connection.source == parentIndex && connection.outlet > object.parentPortIndex)
                            --connection.outlet;
                    }
                    if (inletPort) {
                        --parent.inletCount;
                        parent.signalInlets.erase(parent.signalInlets.begin() + object.parentPortIndex);
                    } else {
                        --parent.outletCount;
                        parent.signalOutlets.erase(parent.signalOutlets.begin() + object.parentPortIndex);
                    }
                    for (auto const siblingIndex : canvas.projectedObjects) {
                        auto& sibling = objects[static_cast<size_t>(siblingIndex)];
                        if (sibling.parentPortIndex > object.parentPortIndex
                            && sibling.pdObject && pd_class(&sibling.pdObject->ob_pd) == pd_class(&object.pdObject->ob_pd))
                            --sibling.parentPortIndex;
                    }
                    break;
                }
            }
            if (object.preState && object.isSubpatch && object.childCanvasIndex >= 0)
                collectSubpatchCanvases(object.childCanvasIndex);
            object.active = false;
            canvas.projectedObjects.erase(std::remove(canvas.projectedObjects.begin(), canvas.projectedObjects.end(), objectIndex),
                canvas.projectedObjects.end());
            canvas.connections.erase(std::remove_if(canvas.connections.begin(), canvas.connections.end(), [&](ProjectedCanvasConnection const& connection) {
                return connection.source == objectIndex || connection.target == objectIndex;
            }), canvas.connections.end());
            if (object.childCanvasIndex >= 0)
                deactivateCanvas(deactivateCanvas, object.childCanvasIndex);
            continue;
        }

        int const sourceIndex = resolveTarget(edit->getProperty("source"), canvasIndex, true);
        if (sourceIndex < 0)
            return false;
        int const targetIndex = resolveTarget(edit->getProperty("target"), canvasIndex, true);
        if (targetIndex < 0)
            return false;
        if (objects[static_cast<size_t>(sourceIndex)].canvasIndex != canvasIndex
            || objects[static_cast<size_t>(targetIndex)].canvasIndex != canvasIndex
            || sourceIndex == targetIndex) {
            error = "InvalidConnection";
            return false;
        }
        if (std::find(movedPortParents.begin(), movedPortParents.end(), sourceIndex) != movedPortParents.end()
            || std::find(movedPortParents.begin(), movedPortParents.end(), targetIndex) != movedPortParents.end()) {
            error = "InvalidConnection";
            return false;
        }

        int const outlet = static_cast<int>(edit->getProperty("outlet"));
        int const inlet = static_cast<int>(edit->getProperty("inlet"));
        if (!validatePorts(sourceIndex, outlet, targetIndex, inlet, op == "connect"))
            return false;

        auto& connections = canvases[static_cast<size_t>(canvasIndex)].connections;
        bool const connected = hasConnection(connections, sourceIndex, outlet, targetIndex, inlet);
        if (op == "connect") {
            if (connected) {
                error = "InvalidConnection";
                return false;
            }
            connections.push_back({ sourceIndex, outlet, targetIndex, inlet });
        } else {
            if (!connected) {
                error = "InvalidConnection";
                return false;
            }
            connections.erase(std::remove_if(connections.begin(), connections.end(), [&](ProjectedCanvasConnection const& connection) {
                return connection.source == sourceIndex && connection.outlet == outlet
                    && connection.target == targetIndex && connection.inlet == inlet;
            }), connections.end());
        }
    }

    return true;
}

bool applyCanvasAdds(t_canvas* root, Array<var> const& edits, var& refMappings, var& identityMappings, bool& dispatched)
{
    struct LiveCanvas {
        t_canvas* canvas = nullptr;
        std::vector<int> preState;
        std::vector<int> objects;
        std::vector<int> beforeAddress;
        std::vector<int> afterAddress;
        std::vector<int> removedPreStateOrdinals;
        bool preStateCanvas = true;
        bool active = true;
        bool hasAfterAddress = false;
    };
    struct LiveObject {
        t_gobj* object = nullptr;
        int canvasIndex = -1;
        int childCanvasIndex = -1;
        std::vector<int> beforeAddress;
        std::vector<int> afterAddress;
        bool preState = true;
        bool subpatch = false;
        bool active = true;
        bool moved = false;
        bool hasAfterAddress = false;
    };
    struct LiveConnection {
        t_outconnect* pointer = nullptr;
        int canvasIndex = -1;
        int sourceIndex = -1;
        int targetIndex = -1;
        int outlet = 0;
        int inlet = 0;
        String pathInfo;
    };
    std::vector<LiveCanvas> canvases;
    std::vector<LiveObject> objects;
    std::vector<LiveConnection> oldConnections;
    std::vector<t_outconnect*> newConnections;
    std::vector<std::pair<String, int>> refs;
    std::unordered_map<t_gobj*, int> objectIds;
    refMappings = var(new DynamicObject());
    identityMappings = var();

    auto collect = [&](auto&& self, t_canvas* canvas, std::vector<int> const& path) -> int {
        int const canvasIndex = static_cast<int>(canvases.size());
        LiveCanvas liveCanvas;
        liveCanvas.canvas = canvas;
        liveCanvas.beforeAddress = path;
        canvases.push_back(liveCanvas);
        int ordinal = 0;
        for (auto* item = canvas->gl_list; item; item = item->g_next, ++ordinal) {
            LiveObject liveObject;
            liveObject.object = item;
            liveObject.canvasIndex = canvasIndex;
            liveObject.subpatch = pd_class(&item->g_pd) == canvas_class;
            liveObject.beforeAddress = path;
            liveObject.beforeAddress.push_back(ordinal);
            int const objectIndex = static_cast<int>(objects.size());
            objects.push_back(liveObject);
            objectIds.emplace(item, objectIndex);
            canvases[static_cast<size_t>(canvasIndex)].preState.push_back(objectIndex);
            canvases[static_cast<size_t>(canvasIndex)].objects.push_back(objectIndex);
            if (liveObject.subpatch) {
                auto childPath = liveObject.beforeAddress;
                int const childIndex = self(self, reinterpret_cast<t_canvas*>(item), childPath);
                objects[static_cast<size_t>(objectIndex)].childCanvasIndex = childIndex;
            }
        }
        return canvasIndex;
    };
    collect(collect, root, {});
    for (size_t index = 0; index < canvases.size(); ++index) {
        t_linetraverser traversal;
        linetraverser_start(&traversal, canvases[index].canvas);
        while (auto* connection = linetraverser_next_nosize(&traversal))
            oldConnections.push_back({ connection, static_cast<int>(index),
                objectIds.at(&traversal.tr_ob->ob_g), objectIds.at(&traversal.tr_ob2->ob_g),
                traversal.tr_outno, traversal.tr_inno,
                traversal.outconnect_path_info != gensym("empty")
                    ? String::fromUTF8(traversal.outconnect_path_info->s_name) : String() });
    }

    auto findRef = [&](String const& name) {
        return std::find_if(refs.begin(), refs.end(), [&name](auto const& entry) { return entry.first == name; });
    };
    auto resolveCanvas = [&](var const& path) {
        int canvasIndex = 0;
        for (auto const& part : *path.getArray()) {
            int objectIndex = -1;
            if (nonnegativeOrdinal(part)) {
                auto const& preState = canvases[static_cast<size_t>(canvasIndex)].preState;
                int const ordinal = static_cast<int>(part);
                if (ordinal < static_cast<int>(preState.size()))
                    objectIndex = preState[static_cast<size_t>(ordinal)];
            } else {
                auto const found = findRef(part.getDynamicObject()->getProperty("ref").toString());
                if (found != refs.end())
                    objectIndex = found->second;
            }
            if (objectIndex < 0)
                return -1;
            auto const& object = objects[static_cast<size_t>(objectIndex)];
            if (!object.active || !object.subpatch || object.childCanvasIndex < 0
                || !canvases[static_cast<size_t>(object.childCanvasIndex)].active)
                return -1;
            canvasIndex = object.childCanvasIndex;
        }
        return canvases[static_cast<size_t>(canvasIndex)].active ? canvasIndex : -1;
    };
    auto resolveTarget = [&](var const& value, int const canvasIndex) {
        int objectIndex = -1;
        if (nonnegativeOrdinal(value)) {
            auto const& preState = canvases[static_cast<size_t>(canvasIndex)].preState;
            int const ordinal = static_cast<int>(value);
            if (ordinal < static_cast<int>(preState.size()))
                objectIndex = preState[static_cast<size_t>(ordinal)];
        } else {
            auto const found = findRef(value.getDynamicObject()->getProperty("ref").toString());
            if (found != refs.end())
                objectIndex = found->second;
        }
        if (objectIndex < 0)
            return -1;
        auto const& object = objects[static_cast<size_t>(objectIndex)];
        return object.active && object.canvasIndex == canvasIndex ? objectIndex : -1;
    };
    auto deactivate = [&](auto&& self, int const objectIndex) -> void {
        auto& object = objects[static_cast<size_t>(objectIndex)];
        if (!object.active)
            return;
        object.active = false;
        objectIds.erase(object.object);
        if (object.childCanvasIndex < 0)
            return;
        auto& child = canvases[static_cast<size_t>(object.childCanvasIndex)];
        if (!child.active)
            return;
        child.active = false;
        for (auto const childObject : child.objects)
            self(self, childObject);
    };

    bool dispatchSucceeded = true;
    struct RemovedWire {
        int sourceIndex, outlet, targetIndex, inlet;
        t_symbol* path;
    };
    std::vector<RemovedWire> removedWires;
    for (auto const& value : edits) {
        auto* edit = value.getDynamicObject();
        int const canvasIndex = resolveCanvas(edit->getProperty("canvas_path"));
        if (canvasIndex < 0) {
            dispatchSucceeded = false;
            break;
        }
        auto* canvas = canvases[static_cast<size_t>(canvasIndex)].canvas;
        auto const op = edit->getProperty("op").toString();
        if (op == "move") {
            int const objectIndex = resolveTarget(edit->getProperty("target"), canvasIndex);
            if (objectIndex < 0) {
                dispatchSucceeded = false;
                break;
            }
            auto& target = objects[static_cast<size_t>(objectIndex)];
            auto* text = Interface::checkObject(target.object);
            if (!text) {
                dispatchSucceeded = false;
                break;
            }
            int const x = static_cast<int>(edit->getProperty("x"));
            int const y = static_cast<int>(edit->getProperty("y"));
            auto* instanceEditor = Interface::getInstanceEditor();
            if (!instanceEditor->canvas_undo_already_set_move)
                canvas_undo_add(canvas, UNDO_MOTION, "motion", canvas_undo_set_move(canvas, 0));
            dispatched = true;
            Interface::moveObject(canvas, target.object, x, y);
            if (pd_class(&target.object->g_pd) == vinlet_class)
                canvas_resortinlets(canvas);
            else if (pd_class(&target.object->g_pd) == voutlet_class)
                canvas_resortoutlets(canvas);
            canvas_dirty(canvas, 1);
            target.moved = true;
            if (text->te_xpix != x || text->te_ypix != y) {
                dispatchSucceeded = false;
                break;
            }
            continue;
        }
        if (op == "remove") {
            int const objectIndex = resolveTarget(edit->getProperty("target"), canvasIndex);
            if (objectIndex < 0) {
                dispatchSucceeded = false;
                break;
            }
            auto* target = objects[static_cast<size_t>(objectIndex)].object;
            if (objects[static_cast<size_t>(objectIndex)].preState) {
                auto& removed = canvases[static_cast<size_t>(canvasIndex)].removedPreStateOrdinals;
                int const ordinal = objects[static_cast<size_t>(objectIndex)].beforeAddress.back();
                if (std::find(removed.begin(), removed.end(), ordinal) == removed.end())
                    removed.push_back(ordinal);
            }
            deactivate(deactivate, objectIndex);
            SmallArray<t_gobj*> targets;
            targets.add(target);
            canvas_undo_add(canvas, UNDO_SEQUENCE_START, "Remove object/s", nullptr);
            dispatched = true;
            Interface::removeObjects(canvas, targets);
            Interface::finishRemove(canvas);
            canvas_undo_add(canvas, UNDO_SEQUENCE_END, "Remove object/s", nullptr);
            continue;
        }
        if (op == "connect" || op == "disconnect") {
            int const sourceIndex = resolveTarget(edit->getProperty("source"), canvasIndex);
            int const targetIndex = resolveTarget(edit->getProperty("target"), canvasIndex);
            if (sourceIndex < 0 || targetIndex < 0 || sourceIndex == targetIndex) {
                dispatchSucceeded = false;
                break;
            }
            auto* source = Interface::checkObject(objects[static_cast<size_t>(sourceIndex)].object);
            auto* target = Interface::checkObject(objects[static_cast<size_t>(targetIndex)].object);
            int const outlet = static_cast<int>(edit->getProperty("outlet"));
            int const inlet = static_cast<int>(edit->getProperty("inlet"));
            if (!source || !target || outlet >= Interface::numOutlets(source) || inlet >= Interface::numInlets(target)
                || (op == "connect" && Interface::isSignalOutlet(source, outlet) && !Interface::isSignalInlet(target, inlet))) {
                dispatchSucceeded = false;
                break;
            }
            bool const connected = Interface::hasConnection(canvas, source, outlet, target, inlet);
            if (op == "connect") {
                if (connected) {
                    dispatchSucceeded = false;
                    break;
                }
                auto const removed = std::find_if(removedWires.rbegin(), removedWires.rend(), [&](auto const& wire) {
                    return wire.sourceIndex == sourceIndex && wire.outlet == outlet
                        && wire.targetIndex == targetIndex && wire.inlet == inlet;
                });
                dispatched = true;
                t_outconnect* connection = nullptr;
                if (removed != removedWires.rend() && removed->path != gensym("empty")) {
                    if (Interface::canConnect(canvas, source, outlet, target, inlet)) {
                        connection = obj_connect(source, outlet, target, inlet);
                        if (connection) {
                            outconnect_set_path_data(connection, removed->path);
                            canvas_undo_add(canvas, UNDO_CONNECT, "connect", canvas_undo_set_connect(canvas,
                                canvas_getindex(canvas, &source->ob_g), outlet, canvas_getindex(canvas, &target->ob_g), inlet, removed->path));
                            canvas_dirty(canvas, 1);
                            glist_noselect(canvas);
                        }
                    }
                } else {
                    connection = Interface::createConnection(canvas, source, outlet, target, inlet);
                }
                if (!connection) {
                    dispatchSucceeded = false;
                    break;
                }
                newConnections.push_back(connection);
            } else {
                if (!connected) {
                    dispatchSucceeded = false;
                    break;
                }
                t_symbol* path = gensym("empty");
                t_linetraverser traversal;
                linetraverser_start(&traversal, canvas);
                while (linetraverser_next_nosize(&traversal))
                    if (traversal.tr_ob == source && traversal.tr_outno == outlet
                        && traversal.tr_ob2 == target && traversal.tr_inno == inlet) {
                        path = traversal.outconnect_path_info;
                        break;
                    }
                removedWires.push_back({ sourceIndex, outlet, targetIndex, inlet, path });
                dispatched = true;
                Interface::removeConnection(canvas, source, outlet, target, inlet, path);
                if (Interface::hasConnection(canvas, source, outlet, target, inlet)) {
                    dispatchSucceeded = false;
                    break;
                }
            }
            continue;
        }
        if (op != "add") {
            dispatchSucceeded = false;
            break;
        }

        std::vector<t_gobj*> before;
        for (auto* item = canvas->gl_list; item; item = item->g_next)
            before.push_back(item);
        auto const type = edit->getProperty("type").toString();
        int const x = static_cast<int>(edit->getProperty("x"));
        int const y = static_cast<int>(edit->getProperty("y"));
        dispatched = true;
        if (type == "subpatch") {
            t_atom subpatch[6], restore[4];
            SETFLOAT(subpatch, 0);
            SETFLOAT(subpatch + 1, 0);
            SETFLOAT(subpatch + 2, 450);
            SETFLOAT(subpatch + 3, 300);
            SETSYMBOL(subpatch + 4, gensym(edit->getProperty("name").toString().toRawUTF8()));
            SETFLOAT(subpatch + 5, 0);
            SETFLOAT(restore, x);
            SETFLOAT(restore + 1, y);
            SETSYMBOL(restore + 2, gensym("pd"));
            restore[3] = subpatch[4];
            canvas_setcurrent(canvas);
            auto* child = canvas_new(nullptr, gensym("canvas"), 6, subpatch);
            pd_typedmess(&child->gl_pd, gensym("restore"), 4, restore);
            canvas_unsetcurrent(canvas);
        } else {
            auto* words = binbuf_new();
            auto const text = edit->getProperty("text").toString();
            binbuf_text(words, text.toRawUTF8(), text.getNumBytesAsUTF8());
            std::vector<t_atom> args(static_cast<size_t>(binbuf_getnatom(words) + 2));
            SETFLOAT(args.data(), x);
            SETFLOAT(args.data() + 1, y);
            std::copy_n(binbuf_getvec(words), binbuf_getnatom(words), args.data() + 2);
            Interface::createObject(canvas, gensym(type == "comment" ? "text" : "obj"), static_cast<int>(args.size()), args.data());
            binbuf_free(words);
        }
        t_gobj* created = nullptr;
        int added = 0;
        for (auto* item = canvas->gl_list; item; item = item->g_next)
            if (std::find(before.begin(), before.end(), item) == before.end()) {
                created = item;
                ++added;
            }
        if (added != 1 || !created) {
            dispatchSucceeded = false;
            break;
        }
        if (type == "subpatch") {
            if (pd_class(&created->g_pd) != canvas_class || reinterpret_cast<t_canvas*>(created)->gl_owner != canvas
                || reinterpret_cast<t_canvas*>(created)->gl_list) {
                dispatchSucceeded = false;
                break;
            }
            canvas_undo_add(canvas, UNDO_CREATE, "create", canvas_undo_set_create(canvas));
            canvas_dirty(canvas, 1);
        } else {
            auto* text = Interface::checkObject(created);
            if (!text || pd_class(&created->g_pd) == canvas_class
                || text->te_type != (type == "comment" ? T_TEXT : T_OBJECT)
                || (type == "object" && pd_class(&created->g_pd) == text_class)) {
                dispatchSucceeded = false;
                break;
            }
        }
        LiveObject createdObject;
        createdObject.object = created;
        createdObject.canvasIndex = canvasIndex;
        createdObject.preState = false;
        createdObject.subpatch = type == "subpatch";
        int const createdIndex = static_cast<int>(objects.size());
        objects.push_back(createdObject);
        objectIds.emplace(created, createdIndex);
        canvases[static_cast<size_t>(canvasIndex)].objects.push_back(createdIndex);
        if (type == "subpatch") {
            LiveCanvas child;
            child.canvas = reinterpret_cast<t_canvas*>(created);
            int const childIndex = static_cast<int>(canvases.size());
            canvases.push_back(child);
            canvases[static_cast<size_t>(childIndex)].preStateCanvas = false;
            objects[static_cast<size_t>(createdIndex)].childCanvasIndex = childIndex;
        }
        refs.emplace_back(edit->getProperty("ref").toString(), createdIndex);
#if ENABLE_TESTING
        if (canvasEditFailureHookForTesting) {
            auto const hook = canvasEditFailureHookForTesting;
            auto* const context = canvasEditFailureContextForTesting;
            canvasEditFailureHookForTesting = nullptr;
            canvasEditFailureContextForTesting = nullptr;
            hook(context);
            dispatchSucceeded = false;
            break;
        }
#endif
    }

    auto collectAfter = [&](auto&& self, t_canvas* canvas, std::vector<int> const& path) -> void {
        int ordinal = 0;
        for (auto* item = canvas->gl_list; item; item = item->g_next, ++ordinal) {
            auto const found = objectIds.find(item);
            if (found != objectIds.end()) {
                auto& object = objects[static_cast<size_t>(found->second)];
                object.afterAddress = path;
                object.afterAddress.push_back(ordinal);
                object.hasAfterAddress = true;
                if (object.childCanvasIndex >= 0) {
                    auto& child = canvases[static_cast<size_t>(object.childCanvasIndex)];
                    child.afterAddress = object.afterAddress;
                    child.hasAfterAddress = true;
                }
            }
            if (pd_class(&item->g_pd) == canvas_class) {
                auto childPath = path;
                childPath.push_back(ordinal);
                self(self, reinterpret_cast<t_canvas*>(item), childPath);
            }
        }
    };
    canvases[0].hasAfterAddress = true;
    collectAfter(collectAfter, root, {});

    auto toAddress = [](std::vector<int> const& address) {
        Array<var> result;
        for (auto const ordinal : address)
            result.add(ordinal);
        return var(result);
    };
    for (auto const& [name, objectIndex] : refs) {
        auto const& object = objects[static_cast<size_t>(objectIndex)];
        if (!object.active || !object.hasAfterAddress)
            continue;
        auto* address = new DynamicObject();
        int const ordinal = object.afterAddress.back();
        auto const pathLength = object.subpatch ? object.afterAddress.size() : object.afterAddress.size() - 1;
        Array<var> path;
        for (size_t index = 0; index < pathLength; ++index)
            path.add(object.afterAddress[index]);
        address->setProperty("canvas_path", var(path));
        address->setProperty("ordinal", ordinal);
        refMappings.getDynamicObject()->setProperty(name, var(address));
    }

    if (dispatched) {
        Array<var> survivors, invalidated;
        for (auto const& object : objects) {
            if (!object.preState)
                continue;
            if (!object.active || !object.hasAfterAddress) {
                bool coveredByAncestor = false;
                for (auto const& ancestor : objects) {
                    if (!ancestor.preState || ancestor.active || !ancestor.subpatch
                        || ancestor.beforeAddress.size() >= object.beforeAddress.size())
                        continue;
                    if (std::equal(ancestor.beforeAddress.begin(), ancestor.beforeAddress.end(), object.beforeAddress.begin())) {
                        coveredByAncestor = true;
                        break;
                    }
                }
                if (coveredByAncestor)
                    continue;
                if (object.subpatch) {
                    auto* subtree = new DynamicObject();
                    subtree->setProperty("before", toAddress(object.beforeAddress));
                    subtree->setProperty("descendants", true);
                    invalidated.add(var(subtree));
                } else {
                    invalidated.add(toAddress(object.beforeAddress));
                }
            } else {
                auto* mapping = new DynamicObject();
                mapping->setProperty("before", toAddress(object.beforeAddress));
                mapping->setProperty("after", toAddress(object.afterAddress));
                mapping->setProperty("moved", object.moved);
                survivors.add(var(mapping));
            }
        }
        Array<var> rebasedCanvases;
        for (auto const& canvas : canvases) {
            if (!canvas.preStateCanvas || !canvas.active || !canvas.hasAfterAddress || canvas.removedPreStateOrdinals.empty())
                continue;
            auto* rebase = new DynamicObject();
            rebase->setProperty("before_canvas_path", toAddress(canvas.beforeAddress));
            rebase->setProperty("after_canvas_path", toAddress(canvas.afterAddress));
            Array<var> removedOrdinals;
            for (auto const ordinal : canvas.removedPreStateOrdinals)
                removedOrdinals.add(ordinal);
            rebase->setProperty("removed_ordinals", var(removedOrdinals));
            rebasedCanvases.add(var(rebase));
        }
        auto* mappings = new DynamicObject();
        Array<var> canvasMappings;
        for (auto const& canvas : canvases)
            if (canvas.preStateCanvas && canvas.active && canvas.hasAfterAddress) {
                auto* mapping = new DynamicObject();
                mapping->setProperty("before", toAddress(canvas.beforeAddress));
                mapping->setProperty("after", toAddress(canvas.afterAddress));
                canvasMappings.add(var(mapping));
            }
        Array<var> connectionMappings, invalidatedConnections, createdConnections, createdCanvases;
        for (auto const& canvas : canvases)
            if (!canvas.preStateCanvas && canvas.active && canvas.hasAfterAddress)
                createdCanvases.add(toAddress(canvas.afterAddress));
        for (auto const& connection : oldConnections) {
            auto const& source = objects[static_cast<size_t>(connection.sourceIndex)];
            auto const& target = objects[static_cast<size_t>(connection.targetIndex)];
            auto const& canvas = canvases[static_cast<size_t>(connection.canvasIndex)];
            auto* before = new DynamicObject();
            before->setProperty("canvas_path", toAddress(canvas.beforeAddress));
            before->setProperty("source", source.beforeAddress.back());
            before->setProperty("outlet", connection.outlet);
            before->setProperty("target", target.beforeAddress.back());
            before->setProperty("inlet", connection.inlet);
            if (connection.pathInfo.isNotEmpty())
                before->setProperty("path_info", connection.pathInfo);
            bool found = false;
            if (source.active && target.active && canvas.active && canvas.hasAfterAddress
                && std::find(newConnections.begin(), newConnections.end(), connection.pointer) == newConnections.end()) {
                t_linetraverser traversal;
                linetraverser_start(&traversal, canvas.canvas);
                while (auto* current = linetraverser_next_nosize(&traversal))
                    if (current == connection.pointer && &traversal.tr_ob->ob_g == source.object
                        && &traversal.tr_ob2->ob_g == target.object) {
                        auto* after = new DynamicObject();
                        after->setProperty("canvas_path", toAddress(canvas.afterAddress));
                        after->setProperty("source", source.afterAddress.back());
                        after->setProperty("outlet", traversal.tr_outno);
                        after->setProperty("target", target.afterAddress.back());
                        after->setProperty("inlet", traversal.tr_inno);
                        if (traversal.outconnect_path_info != gensym("empty"))
                            after->setProperty("path_info", String::fromUTF8(traversal.outconnect_path_info->s_name));
                        auto* mapping = new DynamicObject();
                        mapping->setProperty("before", var(before));
                        mapping->setProperty("after", var(after));
                        connectionMappings.add(var(mapping));
                        found = true;
                        break;
                    }
            }
            if (!found)
                invalidatedConnections.add(var(before));
        }
        for (auto const& canvas : canvases) {
            if (!canvas.active || !canvas.hasAfterAddress)
                continue;
            t_linetraverser traversal;
            linetraverser_start(&traversal, canvas.canvas);
            while (auto* current = linetraverser_next_nosize(&traversal)) {
                bool survived = false;
                for (auto const& old : oldConnections)
                    if (old.pointer == current && old.canvasIndex >= 0
                        && canvases[static_cast<size_t>(old.canvasIndex)].canvas == canvas.canvas
                        && objects[static_cast<size_t>(old.sourceIndex)].active
                        && objects[static_cast<size_t>(old.targetIndex)].active
                        && objects[static_cast<size_t>(old.sourceIndex)].object == &traversal.tr_ob->ob_g
                        && objects[static_cast<size_t>(old.targetIndex)].object == &traversal.tr_ob2->ob_g
                        && std::find(newConnections.begin(), newConnections.end(), current) == newConnections.end()) {
                        survived = true;
                        break;
                    }
                if (survived)
                    continue;
                auto* created = new DynamicObject();
                created->setProperty("canvas_path", toAddress(canvas.afterAddress));
                created->setProperty("source", canvas_getindex(canvas.canvas, &traversal.tr_ob->ob_g));
                created->setProperty("outlet", traversal.tr_outno);
                created->setProperty("target", canvas_getindex(canvas.canvas, &traversal.tr_ob2->ob_g));
                created->setProperty("inlet", traversal.tr_inno);
                if (traversal.outconnect_path_info != gensym("empty"))
                    created->setProperty("path_info", String::fromUTF8(traversal.outconnect_path_info->s_name));
                createdConnections.add(var(created));
            }
        }
        mappings->setProperty("canvases", var(canvasMappings));
        mappings->setProperty("created_canvases", var(createdCanvases));
        mappings->setProperty("connections", var(connectionMappings));
        mappings->setProperty("created_connections", var(createdConnections));
        mappings->setProperty("invalidated_connections", var(invalidatedConnections));
        mappings->setProperty("survivors", var(survivors));
        mappings->setProperty("invalidated", var(invalidated));
        mappings->setProperty("rebased_canvases", var(rebasedCanvases));
        identityMappings = var(mappings);
    }
    return dispatchSucceeded;
}

var makeCanvasUncertain(int const requestId, String const& revision, var const& refs, var const& identityMappings)
{
    auto response = makeDebugError(requestId, "PartialOrUncertain", "Inspect current structure before any retry");
    auto* error = response.getDynamicObject()->getProperty("error").getDynamicObject();
    error->setProperty("revision", revision);
    auto* recovery = new DynamicObject();
    recovery->setProperty("operation", "read_canvas");
    recovery->setProperty("canvas_path", var(Array<var>()));
    recovery->setProperty("expected_revision", revision);
    error->setProperty("recovery", var(recovery));
    if (refs.isObject())
        error->setProperty("refs", refs);
    if (identityMappings.isObject())
        error->setProperty("mappings", identityMappings);
    auto exceedsReplyLimit = [&response] {
        return Base64::toBase64(JSON::toString(response, true)).getNumBytesAsUTF8() > maxEncodedDebugResponseBytes;
    };
    if (exceedsReplyLimit())
        error->removeProperty("refs");
    if (exceedsReplyLimit() && identityMappings.isObject()) {
        auto* mappings = error->getProperty("mappings").getDynamicObject();
        Array<var> movedSurvivors;
        auto const* survivors = mappings->getProperty("survivors").getArray();
        if (survivors)
            for (auto const& survivor : *survivors) {
                auto* entry = survivor.getDynamicObject();
                if (entry && static_cast<bool>(entry->getProperty("moved")))
                    movedSurvivors.add(survivor);
            }
        mappings->setProperty("survivors", var(movedSurvivors));
    }
    if (exceedsReplyLimit())
        error->removeProperty("mappings");
    if (exceedsReplyLimit())
        error->removeProperty("refs");
    return response;
}

bool canvasRecoveryReadable(int const requestId, String const& revision, Array<var> const& records)
{
    String const cursorPrefix = revision + ":" + SHA256(JSON::toString(var(Array<var>())).getCharPointer()).toHexString().substring(0, 16) + ":";
    String const cursor = cursorPrefix + String(records.size());
    for (auto const& record : records) {
        Array<var> singleRecord { record };
        if (Base64::toBase64(JSON::toString(makeCanvasPage(requestId, revision, singleRecord, cursor), true)).getNumBytesAsUTF8()
            > maxEncodedDebugResponseBytes)
            return false;
    }
    return true;
}

var makeCanvasPage(int const requestId, String const& revision, Array<var> const& records, var const& nextCursor)
{
    auto response = makeDebugSuccess(requestId);
    auto* data = response.getDynamicObject()->getProperty("data").getDynamicObject();
    data->setProperty("revision", revision);
    data->setProperty("records", var(records));
    data->setProperty("next_cursor", nextCursor);
    return response;
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

    void addMessage(void* object, String const& message, int type, int64_t id)
    {
        if (consoleMessages.size()) {
            auto& [lastObject, lastMessage, lastType, lastLength, numMessages, lastId] = consoleMessages.back();
            if (object == lastObject && message == lastMessage && type == lastType) {
                numMessages++;
                lastId = id;
            } else {
                consoleMessages.emplace_back(object, message, type, CachedStringWidth<14>::calculateStringWidth(message) + 40, 1, id);
            }
        } else {
            consoleMessages.emplace_back(object, message, type, CachedStringWidth<14>::calculateStringWidth(message) + 40, 1, id);
        }

        lastStoredId = id;

        if (consoleMessages.size() > 800)
            consoleMessages.pop_front();
    }

    void logMessage(void* object, SmallString const& message)
    {
        pendingLock.enter();
        pendingMessages.emplace_back(object, message, 0, nextConsoleSequence++);
        pendingLock.exit();
    }

    void logWarning(void* object, SmallString const& warning)
    {
        pendingLock.enter();
        pendingMessages.emplace_back(object, warning, 1, nextConsoleSequence++);
        pendingLock.exit();
    }

    void logError(void* object, SmallString const& error)
    {
        pendingLock.enter();
        pendingMessages.emplace_back(object, error, 2, nextConsoleSequence++);
        pendingLock.exit();
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

    std::deque<std::tuple<void*, String, int, int, int, int64_t>> consoleMessages;
    std::deque<std::tuple<void*, String, int, int, int, int64_t>> consoleHistory;

    // Sequence ids are assigned exactly once at pending-enqueue under
    // pendingLock and never reset on any clear path. lastStoredId is the
    // high-water mark of ids that reached the stored rows.
    int64 nextConsoleSequence = 1;
    int64 lastStoredId = 0;

    struct DrainSummary {
        int numReceived = 0;
        bool anyWarning = false;
        bool lastWasWarning = false;
        SmallString lastMessage;
    };

    // Moves every pending entry into the visible buffer under the console mutex,
    // returning the summary the GUI notification path needs.
    DrainSummary drainPendingUnderLock()
    {
        DrainSummary summary;
        pendingLock.enter();
        while (!pendingMessages.empty()) {
            auto& [object, message, type, id] = pendingMessages.front();
            addMessage(object, message.toString(), type, id);

            summary.numReceived++;
            summary.anyWarning = summary.anyWarning || (type != 0);
            summary.lastMessage = message;
            summary.lastWasWarning = (type != 0);

            pendingMessages.pop_front();
        }
        pendingLock.exit();
        return summary;
    }

    // Discards every pending entry under the console mutex. This is the
    // clear_console linearization point.
    void discardPendingUnderLock()
    {
        pendingLock.enter();
        pendingMessages.clear();
        pendingLock.exit();
    }

private:
    void timerCallback() override
    {
        auto const summary = drainPendingUnderLock();
        if (summary.numReceived) {
            instance->updateConsole(summary.lastMessage, summary.lastWasWarning, summary.numReceived, summary.anyWarning);
        }
    }

    StackArray<char, 2048> printConcatBuffer = { };

    CriticalSection pendingLock;
    std::deque<std::tuple<void*, SmallString, int, int64_t>> pendingMessages;
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

#if ENABLE_TESTING
void setCanvasEditInterleaveHookForTesting(void (*hook)(void*), void* context)
{
    canvasEditInterleaveHookForTesting = hook;
    canvasEditInterleaveContextForTesting = context;
}
void setCanvasEditFailureHookForTesting(void (*hook)(void*), void* context)
{
    canvasEditFailureHookForTesting = hook;
    canvasEditFailureContextForTesting = context;
}
void setCanvasEditGuiMismatchHookForTesting(void (*hook)(void*), void* context)
{
    canvasEditGuiMismatchHookForTesting = hook;
    canvasEditGuiMismatchContextForTesting = context;
}
void setExportWriteFailureHookForTesting(void (*hook)(void*), void* context)
{
    exportWriteFailureHookForTesting = hook;
    exportWriteFailureContextForTesting = context;
}
#endif

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
                MessageManager::callAsync([inst = juce::WeakReference(inst), canvasToClose = pd::WeakReference(glist, inst)] {
                    if (auto const* pd = static_cast<PluginProcessor*>(inst.get())) {
                        for (auto* editor : pd->getEditors()) {
                            for (auto* canvas : editor->getCanvases()) {
                                auto shouldClose = [&] {
                                    auto canvasPtr = canvas->patch.getPointer();
                                    auto* closingCanvas = canvasToClose.getRaw<t_glist>();
                                    return canvasPtr && closingCanvas && canvasPtr.get() == closingCanvas && !canvasPtr->gl_havewindow;
                                }();
                                if (shouldClose) {
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
    if (!operation.isString()
        || (operation.toString() != "set_generation" && operation.toString() != "send_object"
            && operation.toString() != "get_console" && operation.toString() != "clear_console"
            && operation.toString() != "export_canvas" && operation.toString() != "read_canvas"
            && operation.toString() != "edit_canvas")) {
        reply(makeDebugError(requestId, "UnknownOperation", "Unknown debug operation"), requestId);
        return;
    }

    if (operation.toString() == "read_canvas" || operation.toString() == "edit_canvas") {
        if (!canvasRequestValid(*request, operation.toString() == "edit_canvas")) {
            reply(makeDebugError(requestId, "InvalidRequest", "Canvas request contains invalid, unknown or out-of-bounds fields"), requestId);
            return;
        }
        if (operation.toString() == "edit_canvas") {
            String preflightError;
            Array<var> records;
            String revision;
            String postDispatchRevision;
            String recoveryGeneration = request->getProperty("generation").toString();
            String beforeRevision;
            var refMappings;
            var identityMappings;
            bool dispatched = false;
            struct DisplayedCanvas {
                Component::SafePointer<Canvas> component;
                std::unique_ptr<pd::WeakReference> identity;
                bool inRoot = false;
            };
            std::vector<DisplayedCanvas> displayed;
            for (auto* editor : static_cast<PluginProcessor*>(this)->getEditors()) {
                for (auto* canvas : editor->getCanvases())
                    displayed.push_back({ canvas, nullptr, false });
                if (auto* canvas = editor->getPluginModeCanvas())
                    displayed.push_back({ canvas, nullptr, false });
            }
            Array<pd::Patch::Ptr> subpatchTabsToClose;
            struct TabRestoreState {
                pd::Patch::Ptr patch;
                juce::Component::SafePointer<PluginEditor> editor;
                int splitIndex;
                bool wasVisible;
            };
            std::vector<TabRestoreState> tabsToRestore;
            std::unique_ptr<pd::WeakReference> pinnedRootIdentity;
            auto runEditUnderLock = [&](bool const collectSubpatchTabs) {
                records.clear();
                revision.clear();
                refMappings = var();
                identityMappings = var();
                dispatched = false;
                preflightError.clear();
                if (debugGeneration != request->getProperty("generation").toString()) {
                    preflightError = "StaleGeneration";
                    return;
                }
                if (!debugRoot || !debugRoot->isValid()) {
                    preflightError = "CanvasNotFound";
                    return;
                }

                auto* root = debugRoot->getRawUnchecked<t_canvas>();
                if (pd_class(&root->gl_obj.ob_pd) != canvas_class) {
                    preflightError = "CanvasNotFound";
                    return;
                }
                if (!pinnedRootIdentity) {
                    pinnedRootIdentity = std::make_unique<pd::WeakReference>(root, this);
                } else if (!pinnedRootIdentity->isValid() || pinnedRootIdentity->getRaw<t_canvas>() != root) {
                    preflightError = "StaleGeneration";
                    return;
                }
                for (auto& view : displayed)
                    if (!view.identity && view.component) {
                        if (auto* native = view.component->patch.getRawPointer()) {
                            view.identity = std::make_unique<pd::WeakReference>(native, this);
                            view.inRoot = canvasIsInRoot(root, native);
                        }
                    }
                appendCanvasRecords(root, {}, records);
                revision = SHA256(JSON::toString(var(records), true).getCharPointer()).toHexString();
                if (request->getProperty("expected_revision").toString() != revision) {
                    preflightError = "StaleRevision";
                    return;
                }
                if (!canvasRecoveryReadable(requestId, revision, records)) {
                    preflightError = "RecordTooLarge";
                    return;
                }

                std::vector<t_canvas*> removedSubpatches;
                if (!preflightCanvasEdits(root, *request->getProperty("edits").getArray(), preflightError, removedSubpatches))
                    return;

                if (collectSubpatchTabs) {
                    subpatchTabsToClose.clear();
                    for (auto* removedSubpatch : removedSubpatches) {
                        for (auto const& patch : patches) {
                            if (!patch || patch->getRawPointer() != removedSubpatch)
                                continue;
                            bool const alreadyIncluded = std::any_of(subpatchTabsToClose.begin(), subpatchTabsToClose.end(), [&](auto const& existing) {
                                return existing.get() == patch.get();
                            });
                            if (!alreadyIncluded)
                                subpatchTabsToClose.add(patch);
                        }
                    }
                    if (!subpatchTabsToClose.isEmpty())
                        return;
                }

                if (!applyCanvasAdds(root, *request->getProperty("edits").getArray(), refMappings, identityMappings, dispatched))
                    preflightError = dispatched ? "PartialOrUncertain" : "NotYetAvailable";
                beforeRevision = request->getProperty("expected_revision").toString();
                records.clear();
                appendCanvasRecords(root, {}, records);
                revision = SHA256(JSON::toString(var(records), true).getCharPointer()).toHexString();
            };

            lockAudioThread();
            runEditUnderLock(true);
            bool const closeTabsBeforeDelete = preflightError.isEmpty() && !subpatchTabsToClose.isEmpty();
            unlockAudioThread();

            if (closeTabsBeforeDelete) {
                auto const registrationsToRestore = subpatchTabsToClose;
                tabsToRestore.clear();
                auto const closeResult = MessageManager::callSync([instance = juce::WeakReference(this), subpatchTabsToClose, &tabsToRestore] {
                    auto* processor = static_cast<PluginProcessor*>(instance.get());
                    if (!processor)
                        return false;

                    for (auto const& patch : subpatchTabsToClose) {
                        if (!patch)
                            continue;
                        for (auto* editor : processor->getEditors()) {
                            bool closed = false;
                            for (auto* canvas : editor->getCanvases()) {
                                if (canvas && canvas->patch == *patch) {
                                    bool wasVisible = false;
                                    processor->lockAudioThread();
                                    if (auto* nativeCanvas = patch->getRawPointer())
                                        wasVisible = nativeCanvas->gl_havewindow;
                                    processor->unlockAudioThread();
                                    tabsToRestore.push_back({ patch, editor, patch->splitViewIndex, wasVisible });
                                    canvas->editor->getTabComponent().closeTab(canvas, false);
                                    closed = true;
                                    break;
                                }
                            }
                            if (closed)
                                break;
                        }
                        processor->patches.remove_one(patch, [](auto const& registered, auto const& closing) {
                            return registered.get() == closing.get();
                        });
                    }
                    return true;
                });

                if (!closeResult || !*closeResult) {
                    preflightError = "NotYetAvailable";
                } else {
                    subpatchTabsToClose.clear();
#if ENABLE_TESTING
                    auto const interleaveHook = canvasEditInterleaveHookForTesting;
                    auto* const interleaveContext = canvasEditInterleaveContextForTesting;
                    canvasEditInterleaveHookForTesting = nullptr;
                    canvasEditInterleaveContextForTesting = nullptr;
                    if (interleaveHook)
                        interleaveHook(interleaveContext);
#endif
                    lockAudioThread();
                    runEditUnderLock(false);
                    unlockAudioThread();

                    if (preflightError.isNotEmpty() && !dispatched) {
                        auto const restoreResult = MessageManager::callSync([instance = juce::WeakReference(this),
                                                                               rootIdentity = pinnedRootIdentity.get(),
                                                                               registrationsToRestore, tabsToRestore] {
                            auto* processor = static_cast<PluginProcessor*>(instance.get());
                            if (!processor || !rootIdentity)
                                return false;

                            auto isInOriginalRoot = [](auto&& self, t_canvas* root, t_canvas* candidate) -> bool {
                                if (root == candidate)
                                    return true;
                                for (auto* object = root->gl_list; object; object = object->g_next) {
                                    if (pd_class(&object->g_pd) == canvas_class
                                        && self(self, reinterpret_cast<t_canvas*>(object), candidate))
                                        return true;
                                }
                                return false;
                            };

                            for (auto const& tab : tabsToRestore) {
                                auto* originalEditor = tab.editor.getComponent();
                                if (!originalEditor || originalEditor->editorIndex != tab.patch->windowIndex)
                                    return false;
                            }

                            bool valid = true;
                            processor->lockAudioThread();
                            auto* originalRoot = rootIdentity->getRaw<t_canvas>();
                            valid = originalRoot && pd_class(&originalRoot->gl_obj.ob_pd) == canvas_class;
                            if (valid) {
                                for (auto const& patch : registrationsToRestore) {
                                    auto* nativeCanvas = patch ? patch->getRawPointer() : nullptr;
                                    if (!nativeCanvas || !isInOriginalRoot(isInOriginalRoot, originalRoot, nativeCanvas))
                                        valid = false;
                                }
                            }
                            if (valid) {
                                for (auto const& tab : tabsToRestore) {
                                    auto* nativeCanvas = tab.patch ? tab.patch->getRawPointer() : nullptr;
                                    if (!nativeCanvas || !isInOriginalRoot(isInOriginalRoot, originalRoot, nativeCanvas))
                                        valid = false;
                                }
                            }
                            processor->unlockAudioThread();
                            if (!valid)
                                return false;

                            for (auto const& patch : registrationsToRestore) {
                                processor->patches.add_unique(patch, [](auto const& registered, auto const& restoring) {
                                    return *registered == *restoring;
                                });
                            }

                            for (auto const& tab : tabsToRestore) {
                                auto* targetEditor = tab.editor.getComponent();
                                auto* reopened = targetEditor->getTabComponent().openPatch(tab.patch, false, tab.wasVisible);
                                if (!reopened || reopened->editor != targetEditor)
                                    return false;
                                targetEditor->getTabComponent().showTab(reopened, tab.splitIndex);
                            }
                            bool viewsRestored = true;
                            for (auto const& tab : tabsToRestore) {
                                bool found = false;
                                for (auto* canvas : tab.editor->getCanvases())
                                    if (canvas && canvas->patch == *tab.patch)
                                        found = true;
                                viewsRestored = viewsRestored && found;
                            }

                            processor->lockAudioThread();
                            originalRoot = rootIdentity->getRaw<t_canvas>();
                            valid = originalRoot && pd_class(&originalRoot->gl_obj.ob_pd) == canvas_class;
                            if (valid) {
                                for (auto const& tab : tabsToRestore) {
                                    auto* nativeCanvas = tab.patch->getRawPointer();
                                    if (!nativeCanvas || !isInOriginalRoot(isInOriginalRoot, originalRoot, nativeCanvas)
                                        || static_cast<bool>(nativeCanvas->gl_havewindow) != tab.wasVisible)
                                        valid = false;
                                }
                            }
                            processor->unlockAudioThread();
                            return viewsRestored && valid;
                        });

                        if (!restoreResult || !*restoreResult) {
                            preflightError = "PartialOrUncertain";
                            records.clear();
                            revision.clear();
                            lockAudioThread();
                            if (debugRoot && debugRoot->isValid()) {
                                auto* currentRoot = debugRoot->getRawUnchecked<t_canvas>();
                                if (pd_class(&currentRoot->gl_obj.ob_pd) == canvas_class) {
                                    appendCanvasRecords(currentRoot, {}, records);
                                    revision = SHA256(JSON::toString(var(records), true).getCharPointer()).toHexString();
                                }
                            }
                            unlockAudioThread();
                        }
                    }
                }
            }

            postDispatchRevision = revision;
            if (preflightError.isNotEmpty()) {
                if (preflightError == "PartialOrUncertain") {
                    reply(makeCanvasUncertain(requestId, revision, refMappings, identityMappings), requestId);
                    return;
                }
                String const message = preflightError == "StaleGeneration" ? "Generation does not match the active generation"
                    : preflightError == "StaleRevision" ? "Root structure changed"
                    : preflightError == "RecordTooLarge" ? "Authoritative root readback contains a record larger than the reply bound"
                    : preflightError == "NotYetAvailable" ? "Could not close subpatch tabs before deletion"
                    : "Canvas edit batch failed preflight";
                reply(makeDebugError(requestId, preflightError, message), requestId);
            } else {
                if (!dispatched) {
                    reply(makeDebugError(requestId, "NotYetAvailable", "Canvas editing is not yet available for this batch"), requestId);
                    return;
                }
                auto reconcileGui = [&] {
                    bool valid = true;
                    auto collectOpenedViews = [&] {
                        lockAudioThread();
                        auto* root = pinnedRootIdentity ? pinnedRootIdentity->getRaw<t_canvas>() : nullptr;
                        if (root)
                            for (auto* editor : static_cast<PluginProcessor*>(this)->getEditors()) {
                                auto includeView = [&](Canvas* canvas) {
                                    if (!canvas)
                                        return;
                                    bool const known = std::any_of(displayed.begin(), displayed.end(), [canvas](auto const& view) {
                                        return view.component.getComponent() == canvas;
                                    });
                                    if (!known)
                                        if (auto* native = canvas->patch.getRawPointer(); native && canvasIsInRoot(root, native))
                                            displayed.push_back({ canvas, std::make_unique<pd::WeakReference>(native, this), true });
                                };
                                for (auto* canvas : editor->getCanvases())
                                    includeView(canvas);
                                includeView(editor->getPluginModeCanvas());
                            }
                        unlockAudioThread();
                    };
                    collectOpenedViews();
                    for (auto& view : displayed) {
                        if (!view.inRoot)
                            continue;
                        if (!view.identity || !view.identity->isValid()) {
                            if (view.component)
                                valid = false;
                            continue;
                        }
                        auto* canvas = view.component.getComponent();
                        if (!canvas) {
                            valid = false;
                            continue;
                        }
                        canvas->performSynchronise();
                        if (!view.component)
                            valid = false;
                    }
                    auto const syncedViews = displayed.size();
                    collectOpenedViews();
                    if (displayed.size() != syncedViews)
                        valid = false;
                    std::vector<Component::SafePointer<Canvas>> embeddedViews;
                    auto collectEmbedded = [&](auto&& self, Component* parent) -> void {
                        for (int index = 0; index < parent->getNumChildComponents(); ++index) {
                            Component::SafePointer<Component> child(parent->getChildComponent(index));
                            if (auto* embedded = dynamic_cast<Canvas*>(child.getComponent())) {
                                bool const alreadySeen = std::any_of(embeddedViews.begin(), embeddedViews.end(), [embedded](auto const& view) {
                                    return view.getComponent() == embedded;
                                });
                                if (!alreadySeen) {
                                    embeddedViews.push_back(embedded);
                                    embedded->performSynchronise();
                                }
                            }
                            if (child)
                                self(self, child.getComponent());
                            else
                                valid = false;
                        }
                    };
                    for (auto& view : displayed)
                        if (view.inRoot && view.component)
                            collectEmbedded(collectEmbedded, view.component.getComponent());
#if ENABLE_TESTING
                    if (canvasEditGuiMismatchHookForTesting) {
                        auto const hook = canvasEditGuiMismatchHookForTesting;
                        auto* const context = canvasEditGuiMismatchContextForTesting;
                        canvasEditGuiMismatchHookForTesting = nullptr;
                        canvasEditGuiMismatchContextForTesting = nullptr;
                        hook(context);
                    }
#endif
                    lockAudioThread();
                    auto* root = pinnedRootIdentity ? pinnedRootIdentity->getRaw<t_canvas>() : nullptr;
                    auto* currentRoot = debugRoot ? debugRoot->getRaw<t_canvas>() : nullptr;
                    recoveryGeneration = debugGeneration;
                    valid = valid && root && currentRoot == root
                        && recoveryGeneration == request->getProperty("generation").toString();
                    if (root && currentRoot == root) {
                        records.clear();
                        appendCanvasRecords(root, {}, records);
                        String const currentRevision = SHA256(JSON::toString(var(records), true).getCharPointer()).toHexString();
                        valid = valid && currentRevision == revision;
                        if (currentRevision != postDispatchRevision) {
                            refMappings = var();
                            identityMappings = var();
                        }
                        revision = currentRevision;
                        for (auto& view : displayed)
                            if (view.inRoot && view.identity && view.identity->isValid()) {
                                auto* canvas = view.component.getComponent();
                                valid = valid && canvas && canvas->patch.getRawPointer() == view.identity->getRaw<t_canvas>()
                                    && canvasIsInRoot(root, view.identity->getRaw<t_canvas>())
                                    && canvasGuiMatchesPd(canvas);
                            }
                        for (auto& view : embeddedViews)
                            if (auto* canvas = view.getComponent(); canvas && canvasIsInRoot(root, canvas->patch.getRawPointer()))
                                valid = valid && canvasGuiMatchesPd(canvas);
                            else
                                valid = false;
                    } else {
                        refMappings = var();
                        identityMappings = var();
                        revision.clear();
                        records.clear();
                        if (currentRoot && pd_class(&currentRoot->gl_obj.ob_pd) == canvas_class) {
                            appendCanvasRecords(currentRoot, {}, records);
                            revision = SHA256(JSON::toString(var(records), true).getCharPointer()).toHexString();
                        }
                    }
                    unlockAudioThread();
                    return valid;
                };
                bool guiConverged = false;
                if (MessageManager::getInstance()->isThisTheMessageThread())
                    guiConverged = reconcileGui();
                else if (auto result = MessageManager::callSync(reconcileGui))
                    guiConverged = *result;

                if (guiConverged && canvasRecoveryReadable(requestId, revision, records)) {
                    auto response = makeDebugSuccess(requestId);
                    auto* data = response.getDynamicObject()->getProperty("data").getDynamicObject();
                    data->setProperty("status", "applied");
                    data->setProperty("revision", revision);
                    data->setProperty("pre_revision", beforeRevision);
                    auto* mappings = identityMappings.getDynamicObject();
                    if (!mappings)
                        mappings = new DynamicObject();
                    mappings->setProperty("refs", refMappings);
                    data->setProperty("mappings", var(mappings));
                    data->setProperty("next_cursor", var());
                    if (Base64::toBase64(JSON::toString(response, true)).getNumBytesAsUTF8() <= maxEncodedDebugResponseBytes) {
                        debugCanvasMappingRecords.clear();
                        debugCanvasMappingRevision.clear();
                        debugCanvasMappingGeneration.clear();
                        debugCanvasMappingCursorPrefix.clear();
                        debugCanvasMappingRoot.reset();
                        reply(response, requestId);
                        return;
                    }
                    Array<var> mappingRecords;
                    if (auto* refs = refMappings.getDynamicObject())
                        for (auto const& [name, value] : refs->getProperties()) {
                            auto* record = new DynamicObject();
                            record->setProperty("kind", "ref");
                            record->setProperty("ref", name.toString());
                            record->setProperty("value", value);
                            mappingRecords.add(var(record));
                        }
                    for (auto const* category : { "canvases", "created_canvases", "survivors", "invalidated", "rebased_canvases", "connections", "created_connections", "invalidated_connections" }) {
                        auto const categoryValue = mappings->getProperty(category);
                        if (auto* entries = categoryValue.getArray())
                            for (auto const& value : *entries) {
                                auto* record = new DynamicObject();
                                record->setProperty("kind", category);
                                record->setProperty("value", value);
                                mappingRecords.add(var(record));
                            }
                    }
                    String const prefix = "m:" + SHA256((beforeRevision + ":" + JSON::toString(var(mappingRecords), true)).getCharPointer()).toHexString().substring(0, 16)
                        + ":" + revision + ":";
                    bool pageable = true;
                    for (auto const& record : mappingRecords)
                        if (Base64::toBase64(JSON::toString(makeCanvasPage(requestId, revision, { record }, prefix + String(mappingRecords.size())), true)).getNumBytesAsUTF8()
                            > maxEncodedDebugResponseBytes)
                            pageable = false;
                    if (pageable && !mappingRecords.isEmpty()) {
                        data->setProperty("mappings", var(new DynamicObject()));
                        data->setProperty("next_cursor", prefix + "0");
                        if (Base64::toBase64(JSON::toString(response, true)).getNumBytesAsUTF8() <= maxEncodedDebugResponseBytes) {
                            debugCanvasMappingRecords = mappingRecords;
                            debugCanvasMappingRevision = revision;
                            debugCanvasMappingGeneration = request->getProperty("generation").toString();
                            debugCanvasMappingCursorPrefix = prefix;
                            lockAudioThread();
                            debugCanvasMappingRoot = std::make_unique<pd::WeakReference>(pinnedRootIdentity->getRaw<t_canvas>(), this);
                            unlockAudioThread();
                            reply(response, requestId);
                            return;
                        }
                    }
                }
                auto uncertain = makeCanvasUncertain(requestId, revision, refMappings, identityMappings);
                if (recoveryGeneration != request->getProperty("generation").toString())
                    if (auto* recovery = uncertain.getDynamicObject()->getProperty("error").getDynamicObject()->getProperty("recovery").getDynamicObject())
                        recovery->setProperty("generation", recoveryGeneration);
                auto* error = uncertain.getDynamicObject()->getProperty("error").getDynamicObject();
                if (Base64::toBase64(JSON::toString(uncertain, true)).getNumBytesAsUTF8() > maxEncodedDebugResponseBytes)
                    error->removeProperty("mappings");
                if (Base64::toBase64(JSON::toString(uncertain, true)).getNumBytesAsUTF8() > maxEncodedDebugResponseBytes)
                    error->removeProperty("refs");
                reply(uncertain, requestId);
            }
            return;
        }

        String runtimeError;
        Array<var> allRecords;
        auto const& requestedPath = *request->getProperty("canvas_path").getArray();
        lockAudioThread();
        if (debugGeneration != request->getProperty("generation").toString()) {
            runtimeError = "StaleGeneration";
        } else if (!debugRoot || !debugRoot->isValid()) {
            runtimeError = "CanvasNotFound";
        } else {
            auto* root = debugRoot->getRawUnchecked<t_canvas>();
            if (pd_class(&root->gl_obj.ob_pd) != canvas_class) {
                runtimeError = "CanvasNotFound";
            } else {
                auto* canvas = root;
                for (auto const& component : requestedPath) {
                    auto* object = canvas->gl_list;
                    for (int ordinal = static_cast<int>(component); object && ordinal > 0; --ordinal)
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
                if (runtimeError.isEmpty())
                    appendCanvasRecords(root, {}, allRecords);
            }
        }
        unlockAudioThread();

        if (runtimeError.isNotEmpty()) {
            reply(makeDebugError(requestId, runtimeError, "Canvas target could not be resolved"), requestId);
            return;
        }

        auto const revision = SHA256(JSON::toString(var(allRecords), true).getCharPointer()).toHexString();
        if (request->hasProperty("expected_revision") && request->getProperty("expected_revision").toString() != revision) {
            reply(makeDebugError(requestId, "StaleRevision", "Root structure changed"), requestId);
            return;
        }

        if (request->hasProperty("cursor") && request->getProperty("cursor").toString().startsWith("m:")) {
            auto const cursor = request->getProperty("cursor").toString();
            bool valid = requestedPath.isEmpty() && cursor.startsWith(debugCanvasMappingCursorPrefix)
                && debugCanvasMappingRevision == revision
                && debugCanvasMappingGeneration == request->getProperty("generation").toString();
            lockAudioThread();
            valid = valid && debugCanvasMappingRoot && debugRoot && debugCanvasMappingRoot->isValid()
                && debugRoot->isValid() && debugCanvasMappingRoot->getRaw<t_canvas>() == debugRoot->getRaw<t_canvas>();
            unlockAudioThread();
            if (!valid) {
                reply(makeDebugError(requestId, "StaleRevision", "Mapping no longer belongs to this root revision"), requestId);
                return;
            }
            auto const suffix = cursor.substring(debugCanvasMappingCursorPrefix.length());
            if (suffix.isEmpty() || !suffix.containsOnly("0123456789") || suffix.length() > 10
                || suffix.getLargeIntValue() >= debugCanvasMappingRecords.size()) {
                reply(makeDebugError(requestId, "InvalidRequest", "Invalid mapping cursor"), requestId);
                return;
            }
            Array<var> page;
            for (int index = static_cast<int>(suffix.getLargeIntValue()); index < debugCanvasMappingRecords.size(); ++index) {
                page.add(debugCanvasMappingRecords[index]);
                var const next = index + 1 < debugCanvasMappingRecords.size()
                    ? var(debugCanvasMappingCursorPrefix + String(index + 1)) : var();
                if (Base64::toBase64(JSON::toString(makeCanvasPage(requestId, revision, page, next), true)).getNumBytesAsUTF8() > maxEncodedDebugResponseBytes) {
                    page.removeLast(1);
                    reply(makeCanvasPage(requestId, revision, page, debugCanvasMappingCursorPrefix + String(index)), requestId);
                    return;
                }
            }
            reply(makeCanvasPage(requestId, revision, page, var()), requestId);
            return;
        }

        Array<var> records;
        for (auto const& entry : allRecords) {
            auto* record = entry.getDynamicObject();
            auto const& path = *record->getProperty("canvas_path").getArray();
            if (path.size() < requestedPath.size())
                continue;
            bool inSubtree = true;
            for (int index = 0; index < requestedPath.size(); ++index)
                inSubtree &= static_cast<int>(path[index]) == static_cast<int>(requestedPath[index]);
            if (inSubtree)
                records.add(entry);
        }

        String const cursorPrefix = revision + ":" + SHA256(JSON::toString(var(requestedPath), true).getCharPointer()).toHexString().substring(0, 16) + ":";
        int start = 0;
        if (request->hasProperty("cursor")) {
            auto const cursor = request->getProperty("cursor").toString();
            auto const suffix = cursor.fromFirstOccurrenceOf(cursorPrefix, false, false);
            if (!cursor.startsWith(cursorPrefix) || suffix.isEmpty() || suffix.startsWithChar('0')
                || !suffix.containsOnly("0123456789") || suffix.length() > 10
                || suffix.getLargeIntValue() >= records.size()) {
                reply(makeDebugError(requestId, "InvalidRequest", "Invalid canvas cursor"), requestId);
                return;
            }
            start = static_cast<int>(suffix.getLargeIntValue());
        }

        Array<var> page;
        for (int index = start; index < records.size(); ++index) {
            page.add(records[index]);
            var const next = index + 1 < records.size() ? var(cursorPrefix + String(index + 1)) : var();
            if (Base64::toBase64(JSON::toString(makeCanvasPage(requestId, revision, page, next), true)).getNumBytesAsUTF8() > maxEncodedDebugResponseBytes) {
                page.removeLast(1);
                if (page.isEmpty()) {
                    reply(makeDebugError(requestId, "RecordTooLarge", "Single canvas record exceeds 60 KiB"), requestId);
                    return;
                }
                reply(makeCanvasPage(requestId, revision, page, cursorPrefix + String(index)), requestId);
                return;
            }
        }
        reply(makeCanvasPage(requestId, revision, page, var()), requestId);
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
                    else if (auto* resolvedObject = pd::Interface::checkObject(&object->g_pd); !resolvedObject || (resolvedObject->te_type != T_OBJECT && resolvedObject->te_type != T_MESSAGE))
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

    if (operation.toString() == "get_console") {
        if (!hasOnlyGetConsoleFields(*request)) {
            reply(makeDebugError(requestId, "InvalidRequest", "get_console contains unknown or missing fields"), requestId);
            return;
        }

        auto const includeHistoryValue = request->getProperty("include_history");
        auto const maxEntriesValue = request->getProperty("max_entries");
        if (!includeHistoryValue.isBool()
            || !(maxEntriesValue.isInt() || maxEntriesValue.isInt64())
            || static_cast<int64>(maxEntriesValue) < 1 || static_cast<int64>(maxEntriesValue) > 200) {
            reply(makeDebugError(requestId, "InvalidRequest", "include_history must be boolean and max_entries must be an integer from 1 through 200"), requestId);
            return;
        }

        bool const includeHistory = static_cast<bool>(includeHistoryValue);
        int const maxEntries = static_cast<int>(static_cast<int64>(maxEntriesValue));

        auto const sinceIdValue = request->getProperty("since_id");
        bool const hasSinceId = request->hasProperty("since_id");
        if (hasSinceId && (!(sinceIdValue.isInt() || sinceIdValue.isInt64()) || static_cast<int64>(sinceIdValue) < 0)) {
            reply(makeDebugError(requestId, "InvalidRequest", "since_id must be a non-negative integer"), requestId);
            return;
        }
        int64 const sinceId = hasSinceId ? static_cast<int64>(sinceIdValue) : 0;

        consoleMessageHandler->drainPendingUnderLock();
        int64 const cursor = consoleMessageHandler->lastStoredId;

        auto const& history = consoleMessageHandler->consoleHistory;
        auto const& visible = consoleMessageHandler->consoleMessages;

        Array<var> entries;
        bool truncated = false;

        auto appendEntry = [&](auto const& entry, bool const fromHistory) {
            auto* entryObject = new DynamicObject();
            entryObject->setProperty("text", std::get<1>(entry));
            entryObject->setProperty("severity", consoleSeverityToString(std::get<2>(entry)));
            entryObject->setProperty("repeats", std::get<4>(entry));
            entryObject->setProperty("id", static_cast<int64>(std::get<5>(entry)));
            entryObject->setProperty("source", fromHistory ? "history" : "visible");

            entries.add(var(entryObject));

            if (Base64::toBase64(JSON::toString(makeConsoleSnapshot(requestId, entries, false, cursor), true)).getNumBytesAsUTF8()
                > maxEncodedDebugResponseBytes) {
                entries.removeLast(1);
                truncated = true;
                return false;
            }
            return true;
        };

        if (!hasSinceId) {
            int const historySize = static_cast<int>(history.size());
            int const visibleSize = static_cast<int>(visible.size());
            int const total = includeHistory ? historySize + visibleSize : visibleSize;
            int const emitCount = jmin(maxEntries, total);
            int const startIndex = total - emitCount;

            for (int i = startIndex; i < total; ++i) {
                bool const fromHistory = includeHistory && i < historySize;
                int const index = fromHistory ? i : i - (includeHistory ? historySize : 0);
                auto const& entry = fromHistory ? history[static_cast<size_t>(index)] : visible[static_cast<size_t>(index)];
                if (!appendEntry(entry, fromHistory))
                    break;
            }
        } else {
            int historyEligibleCount = 0;
            Array<std::tuple<void*, String, int, int, int, int64_t> const*> eligible;
            if (includeHistory)
                for (auto const& entry : history)
                    if (std::get<5>(entry) > sinceId) {
                        eligible.add(&entry);
                        ++historyEligibleCount;
                    }

            for (auto const& entry : visible)
                if (std::get<5>(entry) > sinceId)
                    eligible.add(&entry);

            int const total = eligible.size();
            int const emitCount = jmin(maxEntries, total);
            int const startIndex = total - emitCount;

            for (int i = startIndex; i < total; ++i) {
                bool const fromHistory = i < historyEligibleCount;
                auto const& entry = *eligible[i];
                if (!appendEntry(entry, fromHistory))
                    break;
            }
        }

        reply(makeConsoleSnapshot(requestId, entries, truncated, cursor), requestId);
        return;
    }

    if (operation.toString() == "clear_console") {
        if (!hasOnlyClearConsoleFields(*request)) {
            reply(makeDebugError(requestId, "InvalidRequest", "clear_console contains unknown or missing fields"), requestId);
            return;
        }

        consoleMessageHandler->discardPendingUnderLock();

        consoleMessageHandler->consoleMessages.clear();
        consoleMessageHandler->consoleHistory.clear();

        updateConsole(SmallString(), false, 0, false);

        reply(makeClearSuccess(requestId), requestId);
        return;
    }

    if (operation.toString() == "export_canvas") {
        if (!hasOnlyExportCanvasFields(*request)) {
            reply(makeDebugError(requestId, "InvalidRequest", "export_canvas contains unknown or missing fields"), requestId);
            return;
        }

        auto const generationValue = request->getProperty("generation");
        auto const pathValue = request->getProperty("path");
        if (!generationValue.isString()) {
            reply(makeDebugError(requestId, "InvalidRequest", "generation and root_receiver must be strings"), requestId);
            return;
        }

        String const generation = generationValue.toString();
        if (generation.isEmpty() || generation.getNumBytesAsUTF8() > maxGenerationBytes) {
            reply(makeDebugError(requestId, "InvalidRequest", "generation and root_receiver must contain 1 through 128 UTF-8 bytes"), requestId);
            return;
        }

        if (!pathValue.isString()) {
            reply(makeDebugError(requestId, "InvalidRequest", "path must be an absolute file path ending in .png"), requestId);
            return;
        }

        String const path = pathValue.toString();
        if (path.isEmpty() || !File::isAbsolutePath(path) || !path.endsWithIgnoreCase(".png")) {
            reply(makeDebugError(requestId, "InvalidRequest", "path must be an absolute file path ending in .png"), requestId);
            return;
        }

        auto const scaleValue = request->getProperty("scale");
        if ((request->hasProperty("scale") && !(scaleValue.isInt() || scaleValue.isInt64()))
            || (request->hasProperty("scale") && (static_cast<int64>(scaleValue) < 1 || static_cast<int64>(scaleValue) > 4))) {
            reply(makeDebugError(requestId, "InvalidRequest", "scale must be an integer from 1 through 4"), requestId);
            return;
        }
        auto const requestedPath = request->getProperty("canvas_path");
        if (request->hasProperty("canvas_path") && !canvasPathValid(requestedPath, {}, false)) {
            reply(makeDebugError(requestId, "InvalidRequest", "canvas_path must contain at most 32 non-negative integer ordinals"), requestId);
            return;
        }

        // Resolve the registered root under a single Pd lock acquisition,
        // capturing only the pointer value: the captured pointer is never
        // dereferenced after the lock is released, and WeakReference::get()
        // is never called under the lock.
        String runtimeError;
        void* capturedCanvas = nullptr;
        lockAudioThread();
        if (debugGeneration != generation) {
            runtimeError = "StaleGeneration";
        } else if (!debugRoot || !debugRoot->isValid()) {
            runtimeError = "CanvasNotFound";
        } else {
            auto* canvas = debugRoot->getRawUnchecked<t_canvas>();
            if (pd_class(&canvas->gl_obj.ob_pd) != canvas_class)
                runtimeError = "CanvasNotFound";
            else {
                if (auto const* ordinals = requestedPath.getArray()) {
                    for (auto const& component : *ordinals) {
                        auto* object = canvas->gl_list;
                        for (int ordinal = static_cast<int>(component); object && ordinal > 0; --ordinal)
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
                }
                if (runtimeError.isEmpty())
                    capturedCanvas = canvas;
            }
        }
        unlockAudioThread();

        if (runtimeError.isNotEmpty()) {
            auto const message = runtimeError == "StaleGeneration" ? "Generation does not match the active generation" : "Debug target could not be resolved";
            reply(makeDebugError(requestId, runtimeError, message), requestId);
            return;
        }

        // Match the captured pointer value against live canvases on the message
        // thread; the captured value is only ever compared, never dereferenced.
        Canvas* targetCanvas = nullptr;
        auto matchCanvas = [&](auto&& self, Component* component) -> void {
            if (auto* canvas = dynamic_cast<Canvas*>(component)) {
                auto canvasPtr = canvas->patch.getPointer();
                if (canvasPtr && canvasPtr.get() == capturedCanvas)
                    targetCanvas = canvas;
            }
            for (auto* child : component->getChildren()) {
                if (!targetCanvas)
                    self(self, child);
            }
        };
        for (auto* editor : static_cast<PluginProcessor*>(this)->getEditors()) {
            for (auto* canvas : editor->getCanvases()) {
                matchCanvas(matchCanvas, canvas);
                if (targetCanvas) break;
            }
            if (!targetCanvas && editor->getPluginModeCanvas())
                matchCanvas(matchCanvas, editor->getPluginModeCanvas());
            if (targetCanvas)
                break;
        }

        // An existing Canvas must exist for the captured pointer; a never
        // instantiated child or a destroyed root has none. Display state is
        // irrelevant now: inactive tabs, plugin-mode and graph-on-parent
        // canvases all export offscreen.
        if (!targetCanvas) {
            reply(makeDebugError(requestId, "CanvasNotFound", "canvas is not instantiated"), requestId);
            return;
        }

        // Full-content extent in logical canvas units: independent of viewport
        // scroll, editor zoom and desktop DPI.
        String extentReason;
        auto const exportBounds = targetCanvas->getExportBounds(&extentReason);
        if (!exportBounds.has_value() || exportBounds->isEmpty()) {
            reply(makeDebugError(requestId, "ExportFailed", extentReason.isNotEmpty() ? extentReason : "canvas has no visible content"), requestId);
            return;
        }

        int const scale = request->hasProperty("scale") ? static_cast<int>(request->getProperty("scale")) : 1;

        // Output size is the logical extent times the request scale. The bounds
        // are integer logical units, so this multiplication is exact.
        int64 const outputWidth = static_cast<int64>(exportBounds->getWidth()) * scale;
        int64 const outputHeight = static_cast<int64>(exportBounds->getHeight()) * scale;

        // Size caps are checked before any allocation, using int64 arithmetic so
        // the pixel/byte products cannot overflow.
        constexpr int64 maxAxis = 8192;
        constexpr int64 maxPixels = 16777216;
        auto* surface = &targetCanvas->editor->nvgSurface;
        int64 const backendMax = surface->getMaxTextureSize();

        if (outputWidth > maxAxis || outputHeight > maxAxis) {
            reply(makeDebugError(requestId, "ExportTooLarge", "export exceeds the 8192 pixel per-axis limit"), requestId);
            return;
        }
        int64 const totalPixels = outputWidth * outputHeight;
        if (totalPixels > maxPixels) {
            reply(makeDebugError(requestId, "ExportTooLarge", "export exceeds the 16777216 total pixel limit"), requestId);
            return;
        }
        if (backendMax > 0 && (outputWidth > backendMax || outputHeight > backendMax)) {
            reply(makeDebugError(requestId, "ExportTooLarge", "export exceeds the backend texture limit of " + String(backendMax) + " pixels"), requestId);
            return;
        }

        int const width = static_cast<int>(outputWidth);
        int const height = static_cast<int>(outputHeight);
        auto const bounds = *exportBounds;

        bool tooLarge = false;
        auto const image = surface->renderOffscreenImage(width, height,
            [targetCanvas, bounds, width, height, scale](NVGcontext* nvg) {
                nanovg::nvgBeginFrame(nvg, static_cast<float>(width), static_cast<float>(height), 1.0f);
                if (scale > 1)
                    nanovg::nvgScale(nvg, static_cast<float>(scale), static_cast<float>(scale));
                targetCanvas->renderForExport(nvg, bounds);
                nanovg::nvgGlobalScissor(nvg, 0, 0, width, height);
                nanovg::nvgEndFrame(nvg);
            }, tooLarge);
        if (tooLarge) {
            reply(makeDebugError(requestId, "ExportTooLarge", "export exceeds the backend texture limit"), requestId);
            return;
        }
        if (image.isNull()) {
            reply(makeDebugError(requestId, "ExportFailed", "framebuffer readback failed"), requestId);
            return;
        }

        // Publish atomically: encode to a staging file in the destination
        // directory, flush and close it, then rename over the destination only
        // after a completely written image. FileOutputStream appends to an
        // existing file, and deleting the previous export up front would lose
        // good output if encoding then failed, so TemporaryFile stages the new
        // bytes next to the target and replaces it on success. On any failure
        // the staging file is removed and the previous destination bytes are
        // left untouched.
        File const file(path);
        TemporaryFile temp(file);
        {
            FileOutputStream fos(temp.getFile());
            bool failed = !fos.openedOk() || !PNGImageFormat().writeImageToStream(image, fos);
#if ENABLE_TESTING
            if (exportWriteFailureHookForTesting) {
                auto const hook = exportWriteFailureHookForTesting;
                auto* const context = exportWriteFailureContextForTesting;
                exportWriteFailureHookForTesting = nullptr;
                exportWriteFailureContextForTesting = nullptr;
                hook(context);
                failed = true;
            }
#endif
            if (failed) {
                reply(makeDebugError(requestId, "ExportFailed", "failed to write png file"), requestId);
                return;
            }
        }
        if (!temp.overwriteTargetFileWithTemporary()) {
            reply(makeDebugError(requestId, "ExportFailed", "failed to write png file"), requestId);
            return;
        }

        reply(makeExportSuccess(requestId, false), requestId);
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
    debugCanvasMappingRecords.clear();
    debugCanvasMappingRevision.clear();
    debugCanvasMappingGeneration.clear();
    debugCanvasMappingCursorPrefix.clear();
    debugCanvasMappingRoot.reset();

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
    debugCanvasMappingRecords.clear();
    debugCanvasMappingRevision.clear();
    debugCanvasMappingGeneration.clear();
    debugCanvasMappingCursorPrefix.clear();
    debugCanvasMappingRoot.reset();
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

std::deque<std::tuple<void*, String, int, int, int, int64_t>>& Instance::getConsoleMessages() const
{
    return consoleMessageHandler->consoleMessages;
}

std::deque<std::tuple<void*, String, int, int, int, int64_t>>& Instance::getConsoleHistory() const
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
