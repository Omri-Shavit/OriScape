// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Origami diagramming tools; see origami-tools.h.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "origami-tools.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#include "actions/actions-tools.h"
#include "desktop-style.h"
#include "desktop.h"
#include "document-undo.h"
#include "document.h"
#include "layer-manager.h"
#include "message-stack.h"
#include "object/sp-defs.h"
#include "object/sp-item-group.h"
#include "object/sp-use.h"
#include "path-prefix.h"
#include "preferences.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h"
#include "xml/attribute-record.h"
#include "xml/repr.h"

namespace Inkscape::Origami {

namespace {

// Properties that identify a tool's style; a path drawn with all of them unchanged is named
// after the tool.
char const *const SIGNATURE[] = {"fill", "stroke", "stroke-width", "stroke-dasharray",
                                 "marker-start", "marker-end"};

struct Resources
{
    Inkscape::XML::Document *doc = nullptr;
    std::vector<Tool> tools;
    std::map<std::string, Inkscape::XML::Node const *> defs;
};

Resources const &resources()
{
    static std::unique_ptr<Resources> res;
    if (res) {
        return *res;
    }
    res = std::make_unique<Resources>();

    auto const path = std::string(get_inkscape_datadir()) + "/inkscape/origami/origami-tools.svg";
    res->doc = sp_repr_read_file(path.c_str(), SP_SVG_NS_URI);
    if (!res->doc) {
        std::cerr << "Origami tools: could not read " << path << std::endl;
        return *res;
    }

    auto attr = [](Inkscape::XML::Node const *node, char const *name) {
        char const *value = node->attribute(name);
        return std::string(value ? value : "");
    };
    auto ends_with = [](char const *name, std::string const &suffix) {
        std::string const s(name ? name : "");
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    for (auto section = res->doc->root()->firstChild(); section; section = section->next()) {
        if (ends_with(section->name(), ":defs")) {
            for (auto def = section->firstChild(); def; def = def->next()) {
                if (def->attribute("id")) {
                    res->defs[def->attribute("id")] = def;
                }
            }
        } else if (ends_with(section->name(), ":tools")) {
            for (auto node = section->firstChild(); node; node = node->next()) {
                if (!ends_with(node->name(), ":tool")) {
                    continue;
                }
                Tool tool;
                tool.key = attr(node, "key");
                tool.name = attr(node, "name");
                tool.category = attr(node, "category");
                tool.prefix = attr(node, "prefix");
                tool.style = attr(node, "style");
                tool.color = attr(node, "color");
                tool.swatch = attr(node, "swatch");
                tool.symbol = attr(node, "symbol");
                tool.width = std::atof(attr(node, "width").c_str());
                tool.height = std::atof(attr(node, "height").c_str());
                tool.more = attr(node, "more") == "true";
                res->tools.push_back(std::move(tool));
            }
        }
    }
    return *res;
}

struct State
{
    Tool const *active = nullptr;
    SPCSSAttr *applied = nullptr; ///< the active tool's style as written to the desktop
    Inkscape::MessageId message = 0;
    std::shared_ptr<Inkscape::MessageStack> message_stack;
    sigc::signal<void ()> changed;
};

State &state()
{
    static State s;
    return s;
}

void set_message(SPDesktop *desktop, char const *text)
{
    auto &s = state();
    if (s.message_stack && s.message) {
        s.message_stack->cancel(s.message);
    }
    s.message = 0;
    s.message_stack.reset();
    if (desktop && text) {
        s.message_stack = desktop->messageStack();
        s.message = s.message_stack->push(Inkscape::NORMAL_MESSAGE, text);
    }
}

/// Ids of resource defs referenced (url(#id) or href="#id") anywhere in a subtree.
void collect_refs(Inkscape::XML::Node const *node, std::set<std::string> &out)
{
    static std::regex const url(R"(url\(#([^)]+)\))");
    for (auto const &a : node->attributeList()) {
        std::string const value = a.value.pointer() ? a.value.pointer() : "";
        for (std::sregex_iterator it(value.begin(), value.end(), url), end; it != end; ++it) {
            out.insert((*it)[1]);
        }
        if (!value.empty() && value[0] == '#' && std::string(g_quark_to_string(a.key)).find("href") != std::string::npos) {
            out.insert(value.substr(1));
        }
    }
    for (auto child = node->firstChild(); child; child = child->next()) {
        collect_refs(child, out);
    }
}

/// Copy a resource def (and the defs it references) into the document unless it's there.
/// Returns true if anything was added.
bool ensure_def(SPDocument *doc, std::string const &id)
{
    if (doc->getObjectById(id)) {
        return false;
    }
    auto const &defs = resources().defs;
    auto it = defs.find(id);
    if (it == defs.end()) {
        return false;
    }
    std::set<std::string> refs;
    collect_refs(it->second, refs);
    for (auto const &ref : refs) {
        ensure_def(doc, ref);
    }

    auto copy = it->second->duplicate(doc->getReprDoc());
    for (auto const &tool : tools()) {
        if (tool.isPaper() && tool.swatch == id) {
            if (auto stop = copy->firstChild()) {
                stop->setAttribute("style", "stop-color:" + paper_color(tool) + ";stop-opacity:1");
            }
        }
    }
    doc->getDefs()->getRepr()->appendChild(copy);
    Inkscape::GC::release(copy);
    return true;
}

bool ensure_defs_for_style(SPDocument *doc, std::string const &style)
{
    static std::regex const url(R"(url\(#([^)]+)\))");
    bool added = false;
    for (std::sregex_iterator it(style.begin(), style.end(), url), end; it != end; ++it) {
        added = ensure_def(doc, (*it)[1]) || added;
    }
    return added;
}

std::string unique_id(SPDocument *doc, std::string const &prefix)
{
    for (unsigned n = 1;; ++n) {
        auto id = prefix + std::to_string(n);
        if (!doc->getObjectById(id)) {
            return id;
        }
    }
}

void place_symbol(SPDesktop *desktop, Tool const &tool, Geom::Point const &dt_point)
{
    auto doc = desktop->getDocument();
    ensure_def(doc, tool.symbol);

    auto layer = desktop->layerManager().currentLayer();
    auto const to_layer = layer->i2doc_affine().inverse();
    auto const center = dt_point * desktop->dt2doc() * to_layer;
    // The symbol's viewBox is in px; size it in the layer's user units.
    double const units = layer->i2doc_affine().descrim();
    double const w = tool.width / units;
    double const h = tool.height / units;

    auto repr = doc->getReprDoc()->createElement("svg:use");
    repr->setAttribute("xlink:href", "#" + tool.symbol);
    repr->setAttributeSvgDouble("x", center[Geom::X] - w / 2);
    repr->setAttributeSvgDouble("y", center[Geom::Y] - h / 2);
    repr->setAttributeSvgDouble("width", w);
    repr->setAttributeSvgDouble("height", h);
    repr->setAttribute("id", unique_id(doc, tool.prefix));
    layer->appendChildRepr(repr);
    desktop->getSelection()->set(repr);
    Inkscape::GC::release(repr);

    DocumentUndo::done(doc, Glib::ustring::compose(_("Place %1 symbol"), tool.name), "");
}

} // namespace

std::vector<Tool> const &tools()
{
    return resources().tools;
}

Inkscape::XML::Node const *resource_def(std::string const &id)
{
    auto const &defs = resources().defs;
    auto it = defs.find(id);
    return it == defs.end() ? nullptr : it->second;
}

Tool const *active_tool()
{
    return state().active;
}

sigc::signal<void ()> &signal_changed()
{
    return state().changed;
}

std::string paper_color(Tool const &tool)
{
    auto const color = Inkscape::Preferences::get()->getString("/tools/origami/paper/" + tool.key);
    return color.empty() ? tool.color : std::string(color);
}

void set_paper_color(SPDesktop *desktop, Tool const &tool, std::string const &color)
{
    Inkscape::Preferences::get()->setString("/tools/origami/paper/" + tool.key, color);
    if (desktop) {
        auto doc = desktop->getDocument();
        if (auto swatch = doc->getObjectById(tool.swatch)) {
            if (auto stop = swatch->getRepr()->firstChild()) {
                stop->setAttribute("style", "stop-color:" + color + ";stop-opacity:1");
                DocumentUndo::done(doc, Glib::ustring::compose(_("Recolor %1 paper"), tool.name), "");
            }
        }
    }
    state().changed.emit();
}

void pick(SPDesktop *desktop, Tool const *tool)
{
    auto &s = state();
    if (s.applied) {
        sp_repr_css_attr_unref(s.applied);
        s.applied = nullptr;
    }
    s.active = tool;
    set_message(nullptr, nullptr);

    if (!tool || !desktop) {
        s.changed.emit();
        return;
    }

    if (tool->isSymbol()) {
        set_active_tool(desktop, "Select");
        auto text = Glib::ustring::compose(
            _("<b>Click</b> on the canvas to place the %1 symbol. <b>Right-click</b> or <b>Esc</b> to stop."),
            tool->name);
        set_message(desktop, text.c_str());
        s.changed.emit();
        return;
    }

    auto doc = desktop->getDocument();
    auto const current = get_active_tool(desktop);
    bool const drawing = current == "Pen" || current == "Pencil";
    auto selection = desktop->getSelection();
    // While drawing, the selection is just the last path drawn; it keeps its own style.
    if (drawing) {
        selection->clear();
    }
    // Placed symbols stay selected for rotating; restyling them would hide their artwork.
    Inkscape::ObjectSet targets(desktop);
    for (auto item : selection->items()) {
        if (!is<SPUse>(item)) {
            targets.add(item);
        }
    }
    bool const restyle = !targets.isEmpty();

    bool changed = ensure_defs_for_style(doc, tool->style);

    // Like the desktop's current style, tool styles are in px; Inkscape converts to user units.
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, tool->style.c_str());
    sp_desktop_set_style(&targets, desktop, css, restyle, true);
    s.applied = css;

    auto prefs = Inkscape::Preferences::get();
    prefs->setBool("/tools/freehand/pen/usecurrent", true);
    prefs->setBool("/tools/freehand/pencil/usecurrent", true);

    if (changed || restyle) {
        DocumentUndo::done(doc, Glib::ustring::compose(_("Origami tool: %1"), tool->name), "");
    }
    if (!drawing) {
        set_active_tool(desktop, "Pen");
    }
    s.changed.emit();
}

void name_new_path(SPDesktop *desktop, Inkscape::XML::Node *repr)
{
    auto const &s = state();
    if (!s.active || !s.applied || s.active->isSymbol() || !desktop) {
        return;
    }
    for (auto const prop : SIGNATURE) {
        auto const want = std::string(sp_repr_css_property(s.applied, prop, ""));
        auto const have = std::string(sp_repr_css_property(desktop->current, prop, ""));
        if (want != have) {
            return;
        }
    }
    repr->setAttribute("id", unique_id(desktop->getDocument(), s.active->prefix));
}

bool on_canvas_button_press(SPDesktop *desktop, Geom::Point const &world, unsigned button, int num_press)
{
    auto const tool = state().active;
    if (!desktop || !tool || !tool->isSymbol()) {
        return false;
    }
    if (button == 3) {
        pick(desktop, nullptr);
        return true;
    }
    if (button != 1) {
        return false;
    }
    if (num_press == 1) {
        place_symbol(desktop, *tool, desktop->w2d(world));
    }
    return true;
}

bool on_canvas_key_press(SPDesktop *desktop, unsigned keyval)
{
    auto const tool = state().active;
    if (keyval == GDK_KEY_Escape && tool && tool->isSymbol()) {
        pick(desktop, nullptr);
        return true;
    }
    return false;
}

} // namespace Inkscape::Origami

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
