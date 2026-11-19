// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * "Origami tools" page of the Fill and Stroke dialog.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "origami-tools-panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <glibmm/i18n.h>
#include <glibmm/markup.h>
#include <gtkmm/button.h>
#include <gtkmm/colorchooserdialog.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/expander.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/label.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>

#include "color.h"
#include "display/cairo-utils.h"
#include "origami-tools.h"
#include "svg/svg-color.h"
#include "svg/svg.h"
#include "xml/node.h"

namespace Inkscape::UI::Widget {

namespace {

constexpr int PREVIEW_WIDTH = 120;
constexpr int PREVIEW_HEIGHT = 22;

using Style = std::map<std::string, std::string>;

Style parse_style(std::string const &css)
{
    Style style;
    std::stringstream in(css);
    std::string decl;
    while (std::getline(in, decl, ';')) {
        auto colon = decl.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            return s;
        };
        style[trim(decl.substr(0, colon))] = trim(decl.substr(colon + 1));
    }
    return style;
}

std::string get(Style const &style, std::string const &key, std::string const &fallback = "")
{
    auto it = style.find(key);
    return it == style.end() ? fallback : it->second;
}

double get_number(Style const &style, std::string const &key, double fallback)
{
    auto value = get(style, key);
    return value.empty() ? fallback : std::atof(value.c_str());
}

struct Theme
{
    Gdk::RGBA fg;
    Gdk::RGBA bg;
};

Theme theme_of(Gtk::Widget &widget)
{
    Theme theme;
    theme.fg = widget.get_style_context()->get_color(widget.get_state_flags());
    // Light paint is drawn in the opposite of the text color, so white stays "paper" in dark themes.
    double const lum = 0.299 * theme.fg.get_red() + 0.587 * theme.fg.get_green() + 0.114 * theme.fg.get_blue();
    double const inv = lum > 0.5 ? 0.15 : 1.0;
    theme.bg.set_rgba(inv, inv, inv, 1.0);
    return theme;
}

/// Set the source for a diagram paint: dark ink becomes the text color, light paint the background.
bool set_paint(Cairo::RefPtr<Cairo::Context> const &cr, Theme const &theme, std::string const &paint)
{
    if (paint.empty() || paint == "none") {
        return false;
    }
    if (paint.rfind("url(", 0) == 0) {
        auto const &fg = theme.fg;
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.6);
        return true;
    }
    guint32 const rgba = sp_svg_read_color(paint.c_str(), 0x000000ff);
    double const r = SP_RGBA32_R_F(rgba), g = SP_RGBA32_G_F(rgba), b = SP_RGBA32_B_F(rgba);
    auto const &c = (0.299 * r + 0.587 * g + 0.114 * b) > 0.5 ? theme.bg : theme.fg;
    cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(), 1.0);
    return true;
}

void apply_stroke_style(Cairo::RefPtr<Cairo::Context> const &cr, Style const &style, double width)
{
    cr->set_line_width(width);
    auto const cap = get(style, "stroke-linecap", "butt");
    cr->set_line_cap(cap == "round" ? Cairo::LINE_CAP_ROUND : cap == "square" ? Cairo::LINE_CAP_SQUARE : Cairo::LINE_CAP_BUTT);
    auto const join = get(style, "stroke-linejoin", "miter");
    cr->set_line_join(join == "round" ? Cairo::LINE_JOIN_ROUND : join == "bevel" ? Cairo::LINE_JOIN_BEVEL : Cairo::LINE_JOIN_MITER);
}

/// Draw the paths of a marker or symbol, already transformed into place.
void draw_paths(Cairo::RefPtr<Cairo::Context> const &cr, Theme const &theme, Inkscape::XML::Node const *node)
{
    for (auto child = node->firstChild(); child; child = child->next()) {
        if (char const *d = child->attribute("d")) {
            auto const style = parse_style(child->attribute("style") ? child->attribute("style") : "");
            auto const pathv = sp_svg_read_pathv(d);
            cr->begin_new_path();
            feed_pathvector_to_cairo(cr->cobj(), pathv);
            if (set_paint(cr, theme, get(style, "fill", child->attribute("fill") ? child->attribute("fill") : "#000000"))) {
                cr->set_fill_rule(get(style, "fill-rule") == "evenodd" ? Cairo::FILL_RULE_EVEN_ODD : Cairo::FILL_RULE_WINDING);
                cr->fill_preserve();
            }
            if (set_paint(cr, theme, get(style, "stroke", "none"))) {
                double const scale = std::max(std::abs(cr->get_matrix().xx), 1e-6);
                apply_stroke_style(cr, style, std::max(get_number(style, "stroke-width", 1.0), 0.8 / scale));
                cr->stroke_preserve();
            }
        }
        draw_paths(cr, theme, child);
    }
}

void draw_marker(Cairo::RefPtr<Cairo::Context> const &cr, Theme const &theme, std::string const &ref,
                 double x, double y, double width)
{
    static std::regex const url(R"(url\(#([^)]+)\))");
    std::smatch m;
    if (!std::regex_search(ref, m, url)) {
        return;
    }
    if (auto marker = Origami::resource_def(m[1])) {
        cr->save();
        cr->translate(x, y);
        cr->scale(width, width);
        draw_paths(cr, theme, marker);
        cr->restore();
    }
}

void draw_line(Cairo::RefPtr<Cairo::Context> const &cr, Theme const &theme, Origami::Tool const &tool, int w, int h)
{
    auto const style = parse_style(tool.style);
    double const width = get_number(style, "stroke-width", 1.0);
    double const y = std::round(h / 2.0) + 0.5;
    double x0 = 4, x1 = w - 4;

    guint32 const rgba = sp_svg_read_color(get(style, "stroke").c_str(), 0x000000ff);
    if (SP_RGBA32_R_F(rgba) + SP_RGBA32_G_F(rgba) + SP_RGBA32_B_F(rgba) > 1.5) {
        // A white line only shows on paper: draw it over a strip of mid gray.
        cr->set_source_rgb(0.5, 0.5, 0.5);
        cr->rectangle(0, 2, w, h - 4);
        cr->fill();
        cr->set_source_rgb(1, 1, 1);
    } else {
        set_paint(cr, theme, get(style, "stroke"));
    }

    auto const start = get(style, "marker-start", "none");
    auto const end = get(style, "marker-end", "none");
    // Leave room for the heads, which extend past the path ends.
    if (start != "none") x0 += 15 * width;
    if (end != "none") x1 -= 15 * width;

    apply_stroke_style(cr, style, std::max(width, 0.6));
    auto const dashes = get(style, "stroke-dasharray", "none");
    if (dashes != "none") {
        std::vector<double> pattern;
        std::stringstream in(dashes);
        std::string item;
        while (std::getline(in, item, ',')) {
            pattern.push_back(std::atof(item.c_str()));
        }
        cr->set_dash(pattern, 0);
    }
    cr->move_to(x0, y);
    cr->line_to(x1, y);
    cr->stroke();
    cr->unset_dash();

    draw_marker(cr, theme, start, x0, y, width);
    draw_marker(cr, theme, end, x1, y, width);
}

void draw_symbol(Cairo::RefPtr<Cairo::Context> const &cr, Theme const &theme, Origami::Tool const &tool, int w, int h)
{
    auto symbol = Origami::resource_def(tool.symbol);
    if (!symbol || tool.width <= 0 || tool.height <= 0) {
        return;
    }
    double vx = 0, vy = 0;
    if (char const *vb = symbol->attribute("viewBox")) {
        std::sscanf(vb, "%lf %lf", &vx, &vy);
    }
    double const scale = std::min((h - 4) / tool.height, 1.0);
    cr->save();
    cr->translate(4 + (h - tool.width * scale) / 2 * (tool.width < h), (h - tool.height * scale) / 2);
    cr->scale(scale, scale);
    cr->translate(-vx, -vy);
    draw_paths(cr, theme, symbol);
    cr->restore();
}

void draw_paper(Cairo::RefPtr<Cairo::Context> const &cr, Theme const &theme, Origami::Tool const &tool, int h)
{
    guint32 const rgba = sp_svg_read_color(Origami::paper_color(tool).c_str(), 0xffffffff);
    double const x = 4.5, y = 2.5, w = 2.0 * (h - 5), hh = h - 5;
    cr->rectangle(x, y, w, hh);
    cr->set_source_rgb(SP_RGBA32_R_F(rgba), SP_RGBA32_G_F(rgba), SP_RGBA32_B_F(rgba));
    cr->fill_preserve();
    auto const &fg = theme.fg;
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.45);
    cr->set_line_width(1);
    cr->stroke();
}

Glib::ustring category_title(std::string const &category)
{
    if (category == "paper") return _("Paper");
    if (category == "lines") return _("Diagram lines");
    if (category == "arrows") return _("Fold arrows");
    if (category == "symbols") return _("Symbols");
    return category;
}

} // namespace

OrigamiToolsPanel::OrigamiToolsPanel()
    : Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2)
    , _label_group(Gtk::SizeGroup::create(Gtk::SIZE_GROUP_HORIZONTAL))
{
    set_name("OrigamiToolsPanel");
    set_border_width(4);

    auto const &tools = Origami::tools();
    if (tools.empty()) {
        auto label = Gtk::make_managed<Gtk::Label>(_("Origami tool definitions (origami/origami-tools.svg) not found."));
        label->set_line_wrap(true);
        add(*label);
        return;
    }
    _rows.reserve(tools.size());

    auto step_button = Gtk::make_managed<Gtk::Button>(_("Add step"));
    step_button->set_tooltip_text(
        _("Copy the current step, without its arrows, into a new step (Alt+N)"));
    step_button->signal_clicked().connect([this] { Origami::add_step(_desktop); });
    step_button->set_margin_bottom(4);
    add(*step_button);

    std::string category;
    Gtk::Box *section = nullptr;
    Gtk::Box *more = nullptr;
    for (auto const &tool : tools) {
        if (tool.category != category) {
            category = tool.category;
            more = nullptr;
            auto heading = Gtk::make_managed<Gtk::Label>();
            heading->set_markup("<b>" + Glib::Markup::escape_text(category_title(category)) + "</b>");
            heading->set_halign(Gtk::ALIGN_START);
            heading->set_margin_top(section ? 10 : 2);
            heading->set_margin_bottom(2);
            add(*heading);
            section = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
            section->set_margin_start(8);
            add(*section);
        }
        if (tool.more && !more) {
            auto expander = Gtk::make_managed<Gtk::Expander>(_("More"));
            expander->set_expanded(false);
            expander->set_margin_top(2);
            more = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
            expander->add(*more);
            section->pack_end(*expander, false, false);
        }
        (tool.more ? more : section)->add(makeRow(tool));
    }

    _changed = Origami::signal_changed().connect(sigc::mem_fun(*this, &OrigamiToolsPanel::sync));
    show_all_children();
    sync();
}

OrigamiToolsPanel::~OrigamiToolsPanel()
{
    _changed.disconnect();
}

void OrigamiToolsPanel::setDesktop(SPDesktop *desktop)
{
    _desktop = desktop;
}

Gtk::Widget &OrigamiToolsPanel::makeRow(Origami::Tool const &tool)
{
    auto const index = _rows.size();
    auto button = Gtk::make_managed<Gtk::ToggleButton>();
    button->set_relief(Gtk::RELIEF_NONE);
    button->set_focus_on_click(false);

    auto box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto label = Gtk::make_managed<Gtk::Label>(tool.name);
    label->set_halign(Gtk::ALIGN_START);
    label->set_xalign(0);
    _label_group->add_widget(*label);
    box->pack_start(*label, false, false);

    auto preview = Gtk::make_managed<Gtk::DrawingArea>();
    preview->set_size_request(PREVIEW_WIDTH, PREVIEW_HEIGHT);
    preview->set_hexpand(true);
    preview->signal_draw().connect([this, &tool, preview](Cairo::RefPtr<Cairo::Context> const &cr) {
        auto const theme = theme_of(*preview);
        int const w = preview->get_allocated_width();
        int const h = preview->get_allocated_height();
        if (tool.isPaper()) {
            draw_paper(cr, theme, tool, h);
        } else if (tool.isSymbol()) {
            draw_symbol(cr, theme, tool, w, h);
        } else {
            draw_line(cr, theme, tool, w, h);
        }
        return true;
    });
    box->pack_start(*preview, true, true);
    button->add(*box);

    Glib::ustring tip;
    if (tool.isSymbol()) {
        tip = Glib::ustring::compose(_("Click on the canvas to place the %1 symbol (%2)"), tool.name, tool.prefix + "1");
        if (tool.isRecolorable()) {
            tip += _(". Right-click or long-press to change its color.");
        }
    } else if (tool.isPaper()) {
        tip = Glib::ustring::compose(_("Draw %1 paper (%2). Right-click or long-press to change its color."),
                                     tool.name, tool.prefix + "1");
    } else {
        tip = Glib::ustring::compose(_("Draw with the %1 style (%2)"), tool.name, tool.prefix + "1");
    }
    button->set_tooltip_text(tip);

    _rows.push_back({&tool, button, preview, {}});
    button->signal_clicked().connect([this, index] { onClicked(_rows[index]); });

    if (tool.isRecolorable()) {
        button->signal_button_press_event().connect([this, &tool](GdkEventButton *event) {
            if (event->button == 3) {
                editColor(tool);
                return true;
            }
            return false;
        }, false);
        auto long_press = Gtk::GestureLongPress::create(*button);
        long_press->signal_pressed().connect([this, &tool, long_press = long_press.operator->()](double, double) {
            long_press->set_state(Gtk::EVENT_SEQUENCE_CLAIMED);
            editColor(tool);
        });
        _rows.back().long_press = long_press;
    }
    return *button;
}

void OrigamiToolsPanel::onClicked(Row &row)
{
    if (_syncing) {
        return;
    }
    Origami::pick(_desktop, row.tool);
    sync();
}

void OrigamiToolsPanel::editColor(Origami::Tool const &tool)
{
    auto window = dynamic_cast<Gtk::Window *>(get_toplevel());
    auto const title = tool.isPaper() ? Glib::ustring::compose(_("%1 paper color"), tool.name)
                                      : Glib::ustring::compose(_("%1 color"), tool.name);
    Gtk::ColorChooserDialog dialog(title);
    if (window) {
        dialog.set_transient_for(*window);
    }
    dialog.set_use_alpha(false);
    Gdk::RGBA color;
    color.set(Origami::tool_color(tool));
    dialog.set_rgba(color);
    if (dialog.run() == Gtk::RESPONSE_OK) {
        auto const c = dialog.get_rgba();
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", int(c.get_red() * 255 + 0.5),
                      int(c.get_green() * 255 + 0.5), int(c.get_blue() * 255 + 0.5));
        Origami::set_tool_color(_desktop, tool, hex);
    }
}

void OrigamiToolsPanel::sync()
{
    _syncing = true;
    auto const active = Origami::active_tool();
    for (auto &row : _rows) {
        row.button->set_active(row.tool == active);
        row.preview->queue_draw();
    }
    _syncing = false;
}

} // namespace Inkscape::UI::Widget

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
