// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * "Origami tools" page of the Fill and Stroke dialog.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifndef INKSCAPE_UI_WIDGET_ORIGAMI_TOOLS_PANEL_H
#define INKSCAPE_UI_WIDGET_ORIGAMI_TOOLS_PANEL_H

#include <vector>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <sigc++/connection.h>

class SPDesktop;

namespace Gtk {
class DrawingArea;
class GestureLongPress;
class SizeGroup;
class ToggleButton;
} // namespace Gtk

namespace Inkscape::Origami {
struct Tool;
} // namespace Inkscape::Origami

namespace Inkscape::UI::Widget {

class OrigamiToolsPanel : public Gtk::Box
{
public:
    OrigamiToolsPanel();
    ~OrigamiToolsPanel() override;

    void setDesktop(SPDesktop *desktop);

private:
    struct Row
    {
        Origami::Tool const *tool;
        Gtk::ToggleButton *button;
        Gtk::DrawingArea *preview;
        Glib::RefPtr<Gtk::GestureLongPress> long_press;
    };

    Gtk::Widget &makeRow(Origami::Tool const &tool);
    void onClicked(Row &row);
    void editColor(Origami::Tool const &tool);
    void sync();

    SPDesktop *_desktop = nullptr;
    std::vector<Row> _rows;
    Glib::RefPtr<Gtk::SizeGroup> _label_group;
    sigc::connection _changed;
    bool _syncing = false;
};

} // namespace Inkscape::UI::Widget

#endif // INKSCAPE_UI_WIDGET_ORIGAMI_TOOLS_PANEL_H
