// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Gio::Actions for the origami tools.
 *
 * The contents of this file may be used under the GNU General Public License Version 2 or later.
 */

#include "actions-origami.h"

#include <giomm.h>
#include <glibmm/i18n.h>

#include "desktop.h"
#include "inkscape-application.h"
#include "inkscape-window.h"
#include "origami-tools.h"

namespace {

void origami_add_step(SPDesktop *desktop)
{
    Inkscape::Origami::add_step(desktop);
}

std::vector<std::vector<Glib::ustring>> const win_origami_actions = {
    // clang-format off
    {"win.origami-add-step", N_("Add Origami Step"), "Origami", N_("Copy the current step, without its arrows, into a new step")},
    // clang-format on
};

} // namespace

void add_actions_origami(InkscapeWindow *win)
{
    win->add_action("origami-add-step", sigc::bind(sigc::ptr_fun(&origami_add_step), win->get_desktop()));

    if (auto app = InkscapeApplication::instance()) {
        app->get_action_extra_data().add_data(win_origami_actions);
    }
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
