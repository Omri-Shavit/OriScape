// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Origami diagramming tools; see origami-tools.h.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "origami-tools.h"

#include <algorithm>
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
#include "object/sp-page.h"
#include "object/sp-root.h"
#include "object/sp-use.h"
#include "origami-detail.h"
#include "origami-fold.h"
#include "page-manager.h"
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

double const INCH = 96.0;             ///< document coordinates are CSS px
double const STEP_GAP = INCH;         ///< between one step and the next
double const PAGE_MARGIN = INCH / 2;  ///< kept clear at the edges of a page

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
    Inkscape::XML::Node const *named = nullptr; ///< the path name_new_path() last named
    SPGroup *named_step = nullptr;              ///< and the step it belongs in
    std::string step_id;                        ///< the step the user last singled out
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

template <typename F>
void for_each_element(Inkscape::XML::Node *node, F const &f)
{
    for (auto child = node->firstChild(); child; child = child->next()) {
        if (std::string(child->name()).rfind("svg:", 0) == 0) {
            f(child);
            for_each_element(child, f);
        }
    }
}

/// The color a recolorable symbol uses: the user's choice, or the library's.
std::string symbol_color(Tool const &tool)
{
    auto const color = Inkscape::Preferences::get()->getString("/tools/origami/symbol/" + tool.key);
    return color.empty() ? tool.color : std::string(color);
}

/// Recolor a symbol already in the document, and with it every symbol that uses it.
/// Finger: the first stop of its gradient fill (the shaded end). Turn over: the arrows' stroke.
void apply_symbol_color(SPDocument *doc, Tool const &tool, std::string const &color)
{
    auto symbol = doc->getObjectById(tool.symbol);
    if (!symbol) {
        return;
    }
    static std::regex const fill_url(R"(fill:url\(#([^)]+)\))");
    for_each_element(symbol->getRepr(), [&](Inkscape::XML::Node *el) {
        if (std::string(el->name()) != "svg:path") {
            return;
        }
        auto const *style = el->attribute("style");
        std::string const style_str = style ? style : "";
        std::smatch m;
        if (tool.key == "finger" && std::regex_search(style_str, m, fill_url)) {
            auto grad = doc->getObjectById(m[1].str());
            if (!grad) {
                return;
            }
            bool first = true;
            for_each_element(grad->getRepr(), [&](Inkscape::XML::Node *stop) {
                if (first && std::string(stop->name()) == "svg:stop") {
                    first = false;
                    auto css = sp_repr_css_attr(stop, "style");
                    sp_repr_css_set_property(css, "stop-color", color.c_str());
                    sp_repr_css_change(stop, css, "style");
                    sp_repr_css_attr_unref(css);
                }
            });
        } else if (tool.key == "turnOver") {
            auto css = sp_repr_css_attr(el, "style");
            if (sp_repr_css_property(css, "stroke", "none") != std::string("none")) {
                sp_repr_css_set_property(css, "stroke", color.c_str());
                sp_repr_css_change(el, css, "style");
            }
            sp_repr_css_attr_unref(css);
        }
    });
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

void collect_steps(SPObject *parent, std::vector<SPGroup *> &out)
{
    for (auto &child : parent->children) {
        if (step_number(&child)) {
            out.push_back(cast<SPGroup>(&child));
        } else if (is<SPGroup>(&child)) {
            collect_steps(&child, out); // steps live in a layer, which may hold other groups
        }
    }
}

/// Every step in the document, lowest number first.
std::vector<SPGroup *> steps(SPDocument *doc)
{
    std::vector<SPGroup *> out;
    collect_steps(doc->getRoot(), out);
    std::sort(out.begin(), out.end(),
              [](SPGroup *a, SPGroup *b) { return step_number(a) < step_number(b); });
    return out;
}

SPGroup *make_step(SPDocument *doc, int number, SPObject *parent)
{
    auto const name = "step" + std::to_string(number);
    auto repr = doc->getReprDoc()->createElement("svg:g");
    repr->setAttribute("inkscape:label", name);
    repr->setAttribute("id", unique_id(doc, name));
    parent->getRepr()->appendChild(repr);
    Inkscape::GC::release(repr);
    return cast<SPGroup>(doc->getObjectByRepr(repr));
}

/// Shift an item by an offset in document coordinates, through its own transform.
void move_by(SPItem *item, Geom::Point const &offset)
{
    auto const parent = cast<SPItem>(item->parent);
    auto const to_doc = parent ? parent->i2doc_affine() : Geom::Affine(Geom::identity());
    auto const shift = to_doc * Geom::Translate(offset) * to_doc.inverse();
    item->getRepr()->setAttribute("transform", sp_svg_transform_write(item->transform * shift));
}

/// Put a step an inch right of the last one, keeping it on the page: if it would hang off the
/// right edge it starts a new line a half inch in, and if that runs off the bottom, a new page.
void place_step(SPDocument *doc, SPGroup *step, SPGroup *last)
{
    auto const box = step->documentVisualBounds();
    auto const previous = last->documentVisualBounds();
    if (!box || !previous) {
        return; // an empty step has nowhere to be
    }
    auto &pages = doc->getPageManager();
    auto const page = pages.getPageFor(last, false);
    auto const area = page ? page->getDocumentRect() : *doc->preferredBounds();

    Geom::Point at(previous->right() + STEP_GAP, previous->top());
    if (at[Geom::X] + box->width() > area.right() - PAGE_MARGIN) {
        at = Geom::Point(area.left() + PAGE_MARGIN, previous->bottom() + STEP_GAP);
        if (at[Geom::Y] + box->height() > area.bottom() - PAGE_MARGIN) {
            if (auto const fresh = pages.newPage()) {
                doc->ensureUpToDate();
                at = fresh->getDocumentRect().min() + Geom::Point(PAGE_MARGIN, PAGE_MARGIN);
            }
        }
    }
    move_by(step, at - box->min());
}

void place_symbol(SPDesktop *desktop, Tool const &tool, Geom::Point const &dt_point)
{
    auto doc = desktop->getDocument();
    ensure_def(doc, tool.symbol);

    SPItem *container = desktop->layerManager().currentLayer();
    if (!tool.isBetweenSteps()) {
        if (auto const step = current_step(desktop, true)) {
            container = step;
        }
    }
    auto const to_container = container->i2doc_affine().inverse();
    auto const center = dt_point * desktop->dt2doc() * to_container;
    // The symbol's viewBox is in px; size it in the container's user units.
    double const units = container->i2doc_affine().descrim();
    double const w = tool.width / units;
    double const h = tool.height / units;

    auto repr = doc->getReprDoc()->createElement("svg:use");
    repr->setAttribute("xlink:href", "#" + tool.symbol);
    repr->setAttributeSvgDouble("x", center[Geom::X] - w / 2);
    repr->setAttributeSvgDouble("y", center[Geom::Y] - h / 2);
    repr->setAttributeSvgDouble("width", w);
    repr->setAttributeSvgDouble("height", h);
    name_in_step(container, repr, tool);
    container->appendChildRepr(repr);
    desktop->getSelection()->set(repr);
    Inkscape::GC::release(repr);

    DocumentUndo::done(doc, Glib::ustring::compose(_("Place %1 symbol"), tool.name), "");
}

} // namespace

std::string object_name(SPObject const *object)
{
    if (auto const label = object->label()) {
        return label;
    }
    if (auto const id = object->getId()) {
        return id;
    }
    return {};
}

int step_number(SPObject const *object)
{
    static std::regex const re(R"(step(\d+))");
    std::smatch match;
    auto const name = object_name(object);
    if (!is<SPGroup>(object) || !std::regex_match(name, match, re)) {
        return 0;
    }
    return std::atoi(match[1].str().c_str());
}

Tool const *tool_of(SPObject const *object)
{
    static std::regex const number(R"(\d+(-\d+)?)");
    auto const name = object_name(object);
    for (auto const &tool : tools()) {
        if (name.size() > tool.prefix.size() && name.compare(0, tool.prefix.size(), tool.prefix) == 0 &&
            std::regex_match(name.substr(tool.prefix.size()), number)) {
            return &tool;
        }
    }
    return nullptr;
}

Tool const *tool_by_key(std::string const &key)
{
    for (auto const &tool : tools()) {
        if (tool.key == key) {
            return &tool;
        }
    }
    return nullptr;
}

void name_in_step(SPObject *step, Inkscape::XML::Node *repr, Tool const &tool)
{
    int last = 0;
    for (auto &child : step->children) {
        auto const name = object_name(&child);
        if (name.size() > tool.prefix.size() && name.compare(0, tool.prefix.size(), tool.prefix) == 0) {
            last = std::max(last, std::atoi(name.c_str() + tool.prefix.size()));
        }
    }
    auto const name = tool.prefix + std::to_string(last + 1);
    repr->setAttribute("inkscape:label", name);
    repr->setAttribute("id", unique_id(step->document, name));
}

SPObject *naming_scope(SPObject *object)
{
    for (auto scope = object; scope; scope = scope->parent) {
        if (step_number(scope)) {
            return scope;
        }
    }
    return object->parent ? object->parent : object;
}

std::string unique_id(SPDocument *doc, std::string const &name)
{
    if (!doc->getObjectById(name)) {
        return name;
    }
    for (int n = 2;; ++n) {
        auto id = name + "-" + std::to_string(n);
        if (!doc->getObjectById(id)) {
            return id;
        }
    }
}

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
    for (auto const &tool : tools()) {
        if (tool.isSymbol() && tool.symbol == id) {
            apply_symbol_color(doc, tool, symbol_color(tool));
        }
    }
    return true;
}

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

std::string tool_color(Tool const &tool)
{
    return tool.isPaper() ? paper_color(tool) : symbol_color(tool);
}

void set_tool_color(SPDesktop *desktop, Tool const &tool, std::string const &color)
{
    if (tool.isPaper()) {
        set_paper_color(desktop, tool, color);
        return;
    }
    Inkscape::Preferences::get()->setString("/tools/origami/symbol/" + tool.key, color);
    if (desktop) {
        auto doc = desktop->getDocument();
        ensure_def(doc, tool.symbol);
        apply_symbol_color(doc, tool, color);
        DocumentUndo::done(doc, Glib::ustring::compose(_("Recolor %1"), tool.name), "");
    }
    state().changed.emit();
}

void pick(SPDesktop *desktop, Tool const *tool)
{
    cancel_fold(); // reaching for a tool is not the click the fold command is waiting for
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

    current_step(desktop, false); // note the step on show now; drawing will clear the selection

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
    // While drawing, the selection is just the last path drawn, which keeps its own style. A
    // selected step is different: it says where the next object goes, so it stays selected.
    if (drawing) {
        std::vector<SPItem *> keep;
        for (auto const item : selection->items()) {
            if (step_number(item)) {
                keep.push_back(item);
            }
        }
        selection->setList(keep);
    }
    // Placed symbols stay selected for rotating; restyling them would hide their artwork. A
    // selected step says where the next object goes, so it is not something to restyle either.
    Inkscape::ObjectSet targets(desktop);
    for (auto item : selection->items()) {
        if (!is<SPUse>(item) && !step_number(item)) {
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
    auto &s = state();
    s.named = nullptr;
    s.named_step = nullptr;
    if (!s.active || !s.applied || s.active->isSymbol() || !desktop) {
        return;
    }
    for (auto const prop : SIGNATURE) {
        auto const want = std::string(sp_repr_css_property(s.applied, prop, ""));
        auto const have = std::string(sp_repr_css_property(desktop->current, prop, ""));
        if (want != have) {
            return; // drawn with a style of the user's own, so not ours to name
        }
    }
    auto const step = current_step(desktop, true);
    if (!step) {
        return;
    }
    name_in_step(step, repr, *s.active);
    s.named = repr;
    s.named_step = step;
}

SPGroup *container_for(Inkscape::XML::Node const *repr, SPGroup *layer)
{
    auto const &s = state();
    return repr && repr == s.named && s.named_step ? s.named_step : layer;
}

SPGroup *current_step(SPDesktop *desktop, bool create)
{
    if (!desktop) {
        return nullptr;
    }
    auto &s = state();
    auto const doc = desktop->getDocument();
    for (auto const item : desktop->getSelection()->items()) {
        for (SPObject *object = item; object; object = object->parent) {
            if (step_number(object)) {
                s.step_id = object->getId() ? object->getId() : "";
                return cast<SPGroup>(object);
            }
        }
    }
    // The drawing tools drop the selection as they begin a path, so the step the user singled
    // out before reaching for a tool is remembered until they single out another one.
    if (!s.step_id.empty()) {
        auto const remembered = cast<SPGroup>(doc->getObjectById(s.step_id));
        if (remembered && step_number(remembered)) {
            return remembered;
        }
        s.step_id.clear();
    }
    auto const all = steps(doc);
    if (!all.empty()) {
        return all.back();
    }
    return create ? make_step(doc, 1, desktop->layerManager().currentLayer()) : nullptr;
}

SPGroup *add_step(SPDesktop *desktop)
{
    if (!desktop) {
        return nullptr;
    }
    auto doc = desktop->getDocument();
    auto const all = steps(doc);
    auto const source = all.empty() ? nullptr : current_step(desktop, false);
    auto const step = make_step(doc, all.empty() ? 1 : step_number(all.back()) + 1,
                                source ? source->parent : desktop->layerManager().currentLayer());
    if (!step) {
        return nullptr;
    }

    if (source) {
        auto xml = doc->getReprDoc();
        for (auto child = source->getRepr()->firstChild(); child; child = child->next()) {
            auto const object = doc->getObjectByRepr(child);
            auto const tool = object ? tool_of(object) : nullptr;
            if (tool && tool->isArrow()) {
                continue; // an arrow describes the fold about to happen, so it stays behind
            }
            auto copy = child->duplicate(xml);
            // Names repeat from step to step, so give the copy a free id to go with its label.
            if (auto const label = copy->attribute("inkscape:label")) {
                copy->setAttribute("id", unique_id(doc, label));
            }
            step->getRepr()->appendChild(copy);
            Inkscape::GC::release(copy);
        }
        doc->ensureUpToDate();
        place_step(doc, step, all.back());
    }

    desktop->getSelection()->set(step);
    DocumentUndo::done(doc, Glib::ustring::compose(_("Add %1"), object_name(step)), "");
    return step;
}

bool on_canvas_button_press(SPDesktop *desktop, Geom::Point const &world, unsigned button, int num_press)
{
    if (fold_on_button_press(desktop, world, button)) {
        return true;
    }
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

bool on_canvas_motion(SPDesktop *desktop, Geom::Point const &world)
{
    return fold_on_motion(desktop, world);
}

bool on_canvas_key_press(SPDesktop *desktop, unsigned keyval)
{
    if (fold_on_key_press(desktop, keyval)) {
        return true;
    }
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
