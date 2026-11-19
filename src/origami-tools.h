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
 *
 * Everything drawn or stamped with a tool goes into a step: a group named "step1", "step2", ...
 * Objects are named after their tool and numbered within their step, so each step has its own
 * mountainFold1, mountainFold2 and so on. The name is the inkscape:label the Objects dialog
 * shows; ids stay unique, as SVG requires. add_step() copies a step into the next one.
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
class SPGroup;

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
    bool isArrow() const { return category == "arrows"; }
    /// Turn over says what happens between two steps, so it lives outside them and is not copied.
    bool isBetweenSteps() const { return key == "turnOver"; }
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

/// Name a path the pen/pencil is about to add after the active tool ("mountainFold3"), if it
/// was drawn with that tool's style. Remembers the step it belongs in for container_for().
void name_new_path(SPDesktop *desktop, Inkscape::XML::Node *repr);

/// Where the pen/pencil should put a path it just made: the step it was named for, else the
/// layer it would have gone into anyway.
SPGroup *container_for(Inkscape::XML::Node const *repr, SPGroup *layer);

/// The step new objects go into: the step holding the selection, else the last step.
/// Makes step1 when the document has no steps and @a create is set.
SPGroup *current_step(SPDesktop *desktop, bool create);

/// Copy the current step, without its arrows, into a new step after the last one: an inch to
/// its right, else at the start of a new line, else on a new page. Returns the new step.
SPGroup *add_step(SPDesktop *desktop);

/// Canvas hooks for the symbol stamp; return true if the event was consumed.
bool on_canvas_button_press(SPDesktop *desktop, Geom::Point const &world, unsigned button, int num_press);
bool on_canvas_key_press(SPDesktop *desktop, unsigned keyval);

} // namespace Inkscape::Origami

#endif // INKSCAPE_ORIGAMI_TOOLS_H
