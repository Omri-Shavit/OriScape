// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Applying a fold: turning a crease into folded paper.
 *
 * With one mountain or valley crease selected, apply_fold() arms the command: the user points at
 * the half of the paper to fold, sees a curved arrow showing where it will land, and clicks. The
 * paper under the crease is then divided along the line through the crease's endpoints, the half
 * that was picked is mirrored onto the other, and the two halves are stacked the way the crease
 * says: the mirrored half on top for a valley, underneath for a mountain. Everything else drawn
 * on that paper is divided and mirrored with it, mountains becoming valleys and the other way
 * about, and whatever ends up hidden under the paper on top is trimmed away or dropped.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_ORIGAMI_FOLD_H
#define INKSCAPE_ORIGAMI_FOLD_H

#include <2geom/point.h>

class SPDesktop;

namespace Inkscape::Origami {

/// Arm the fold command for the selected crease; the next canvas click applies it.
void apply_fold(SPDesktop *desktop);

/// Stop waiting for that click and take the preview arrow down.
void cancel_fold();

/// Canvas hooks while armed; each returns true if it consumed the event.
bool fold_on_motion(SPDesktop *desktop, Geom::Point const &world);
bool fold_on_button_press(SPDesktop *desktop, Geom::Point const &world, unsigned button);
bool fold_on_key_press(SPDesktop *desktop, unsigned keyval);

} // namespace Inkscape::Origami

#endif // INKSCAPE_ORIGAMI_FOLD_H

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
