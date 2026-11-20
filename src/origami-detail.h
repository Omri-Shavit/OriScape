// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Naming and resource helpers shared by the origami tool palette and the fold command.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_ORIGAMI_DETAIL_H
#define INKSCAPE_ORIGAMI_DETAIL_H

#include <string>

#include "origami-tools.h"

class SPDocument;
class SPObject;

namespace Inkscape::XML {
class Node;
} // namespace Inkscape::XML

namespace Inkscape::Origami {

/// What the Objects dialog shows for an object: its label, else its id.
std::string object_name(SPObject const *object);

/// n for a group named "step<n>", 0 for anything else.
int step_number(SPObject const *object);

/// The tool an object was made with, going by its name: "mountainFold3" is a Mountain.
Tool const *tool_of(SPObject const *object);

/// The tool with this key ("valley", "gray", ...), or nullptr.
Tool const *tool_by_key(std::string const &key);

/// Name a new object after its tool, numbered within @a step: "mountainFold7".
void name_in_step(SPObject *step, Inkscape::XML::Node *repr, Tool const &tool);

/// The step an object belongs to for naming: the step it is in, else its parent.
SPObject *naming_scope(SPObject *object);

/// An id nobody is using: the name itself, else the name with "-2", "-3" ... appended.
std::string unique_id(SPDocument *doc, std::string const &name);

/// Copy a resource def (and the defs it references) into the document unless it's there.
/// Returns true if anything was added.
bool ensure_def(SPDocument *doc, std::string const &id);

} // namespace Inkscape::Origami

#endif // INKSCAPE_ORIGAMI_DETAIL_H

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
