// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Origami diagramming tools: paper swatches, crease lines, fold arrows and symbols.
 *
 * The tool definitions live in share/origami/origami-tools.svg, generated from the
 * diagrammer's style library by share/origami/generate_origami_tools.py.
 *
 * Picking a paper, line or arrow tool makes its style the desktop's current style (the one the
 * pen and pencil tools draw with) and restyles any selected objects. Picking a symbol arms a
 * stamp: each left click on the canvas places that symbol until right-click or Escape.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_ORIGAMI_TOOLS_H
#define INKSCAPE_ORIGAMI_TOOLS_H

#include <string>
#include <vector>
#include <2geom/point.h>
#include <sigc++/signal.h>

class SPDesktop;

namespace Inkscape::XML {
class Node;
} // namespace Inkscape::XML

namespace Inkscape::Origami {

struct Tool
{
    std::string key;      ///< e.g. "mountain"
    std::string name;     ///< shown in the UI, e.g. "Mountain"
    std::string category; ///< "paper", "lines", "arrows" or "symbols"
    std::string prefix;   ///< id prefix of objects drawn with this tool, e.g. "mountainFold"
    std::string style;    ///< CSS in px (empty for symbols)
    std::string color;    ///< default paper color
    std::string swatch;   ///< id of the paper swatch gradient
    std::string symbol;   ///< id of the <symbol> placed by a symbol tool
    double width = 0;     ///< symbol size in px
    double height = 0;
    bool more = false;    ///< listed under the category's "More" expander

    bool isSymbol() const { return category == "symbols"; }
    bool isPaper() const { return category == "paper"; }
    /// Tools whose color the user can change with a long press: paper, finger and turn over.
    bool isRecolorable() const { return isPaper() || key == "finger" || key == "turnOver"; }
};

/// All tools in palette order; empty (with a warning printed) if the resource file is missing.
std::vector<Tool> const &tools();

/// Definition (swatch, marker, symbol) from the resource file, or nullptr.
Inkscape::XML::Node const *resource_def(std::string const &id);

/// The tool picked last, or nullptr.
Tool const *active_tool();

/// Emitted when the active tool or a paper color changes.
sigc::signal<void ()> &signal_changed();

/// Pick a tool: see the file comment. Passing nullptr deactivates the current tool.
void pick(SPDesktop *desktop, Tool const *tool);

/// The color a paper tool's swatch is created with (user override or the library's).
std::string paper_color(Tool const &tool);

/// Change a paper tool's color; recolors the document's swatch, and with it everything using it.
void set_paper_color(SPDesktop *desktop, Tool const &tool, std::string const &color);

/// The current color of a recolorable tool: paper color, finger shade, or turn-over stroke.
std::string tool_color(Tool const &tool);

/// Recolor a recolorable tool (see isRecolorable). Symbols keep their gradient: a finger's
/// color is its gradient's shaded stop. Every placed symbol of that tool updates.
void set_tool_color(SPDesktop *desktop, Tool const &tool, std::string const &color);

/// Give a path the pen/pencil is about to add an id like "mountainFold3", if it was drawn
/// with the active tool's style.
void name_new_path(SPDesktop *desktop, Inkscape::XML::Node *repr);

/// Canvas hooks for the symbol stamp; return true if the event was consumed.
bool on_canvas_button_press(SPDesktop *desktop, Geom::Point const &world, unsigned button, int num_press);
bool on_canvas_key_press(SPDesktop *desktop, unsigned keyval);

} // namespace Inkscape::Origami

#endif // INKSCAPE_ORIGAMI_TOOLS_H
