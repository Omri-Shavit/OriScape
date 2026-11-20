// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * Applying a fold; see origami-fold.h.
 */
/*
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "origami-fold.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <2geom/path.h>
#include <2geom/pathvector.h>
#include <2geom/transforms.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>

#include "desktop.h"
#include "display/control/canvas-item-bpath.h"
#include "display/control/canvas-item-ptr.h"
#include "display/curve.h"
#include "document-undo.h"
#include "document.h"
#include "livarot/LivarotDefs.h"
#include "message-stack.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "object/sp-shape.h"
#include "origami-detail.h"
#include "path/path-boolop.h"
#include "selection.h"
#include "style.h"
#include "svg/svg.h"
#include "xml/repr.h"

namespace Inkscape::Origami {

namespace {

double const INCH = 96.0;                ///< document coordinates are CSS px
double const POINT = INCH / 72.0;
double const NOTCH = 7.0 * M_PI / 180.0; ///< base angle of the notch that hides a doubled edge
double const PULL_BACK = 2 * POINT;      ///< how far a crease backs away from what hides it
double const HIDDEN = 0.98;              ///< a piece covered this much counts as out of sight
double const SLIVER = 1e-3;              ///< area below which a piece of paper is nothing at all
double const CRUMB = 0.05;               ///< length below which a piece of line is nothing at all
double const TINY = 1e-7;
int const SAMPLES = 12;                  ///< points tried along a path when asking what it lies on

/// The ant's right as it crawls along the crease: a quarter turn clockwise, y running down.
Geom::Point right_of(Geom::Point const &heading)
{
    return Geom::Point(-heading[Geom::Y], heading[Geom::X]);
}

/// An item's geometry in document coordinates; empty for anything that is not a shape.
Geom::PathVector item_path(SPItem *item)
{
    auto const shape = cast<SPShape>(item);
    if (!shape || !shape->curve()) {
        return {};
    }
    return shape->curve()->get_pathvector() * item->i2doc_affine();
}

/// Area enclosed, sampling curves finely enough for the coverage tests below.
double area(Geom::PathVector const &pathv)
{
    double total = 0;
    for (auto const &path : pathv) {
        int const steps = std::max<int>(16, path.size() * 8);
        auto previous = path.initialPoint();
        for (int i = 1; i <= steps; ++i) {
            auto const point = path.pointAt(path.size() * double(i) / steps);
            total += Geom::cross(previous, point);
            previous = point;
        }
        total += Geom::cross(previous, path.initialPoint());
    }
    return std::abs(total) / 2;
}

bool inside(Geom::PathVector const &pathv, Geom::Point const &point)
{
    return pathv.winding(point) != 0;
}

double span(Geom::PathVector const &pathv)
{
    auto const box = pathv.boundsFast();
    return box ? box->dimensions().length() : 0;
}

/// Everything on one side of the line through @a origin facing @a normal, as a path to cut with.
Geom::PathVector half_plane(Geom::Point const &origin, Geom::Point const &normal, double reach)
{
    auto const out = Geom::unit_vector(normal);
    auto const along = Geom::rot90(out) * reach;
    Geom::Path path(origin - along);
    path.appendNew<Geom::LineSegment>(origin + along);
    path.appendNew<Geom::LineSegment>(origin + along + out * reach);
    path.appendNew<Geom::LineSegment>(origin - along + out * reach);
    path.close();
    return Geom::PathVector(path);
}

Geom::PathVector combine(Geom::PathVector const &a, Geom::PathVector const &b, BooleanOp op)
{
    if (a.empty()) {
        return op == bool_op_union ? b : Geom::PathVector();
    }
    if (b.empty()) {
        return op == bool_op_inters ? Geom::PathVector() : a;
    }
    return sp_pathvector_boolop(a, b, op, fill_nonZero, fill_nonZero);
}

/// How much of @a piece lies under @a cover.
bool covered_by(Geom::PathVector const &piece, Geom::PathVector const &cover, double size)
{
    return area(combine(piece, cover, bool_op_inters)) > HIDDEN * size;
}

/// How far it is to walk @a path from one end to the other.
double length_of(Geom::Path const &path)
{
    double total = 0;
    for (auto const &curve : path) {
        total += curve.length();
    }
    return total;
}

/// Split @a path wherever it crosses @a cutter, in order along the path.
std::vector<Geom::Path> divide(Geom::Path const &path, Geom::PathVector const &cutter)
{
    std::vector<double> cuts{0.0};
    for (auto const &other : cutter) {
        for (auto const &crossing : path.intersect(other)) {
            cuts.push_back(crossing.first.asFlatTime());
        }
    }
    cuts.push_back(path.size());
    std::sort(cuts.begin(), cuts.end());

    // An edge the path ends on or runs beside crosses it a hair early, which would otherwise
    // leave a crumb of a piece behind and make the line look more divided than it looks.
    std::vector<double> kept{cuts.front()};
    for (size_t i = 1; i + 1 < cuts.size(); ++i) {
        if (length_of(path.portion(kept.back(), cuts[i])) > CRUMB) {
            kept.push_back(cuts[i]);
        }
    }
    while (kept.size() > 1 && length_of(path.portion(kept.back(), cuts.back())) <= CRUMB) {
        kept.pop_back();
    }
    kept.push_back(cuts.back());

    std::vector<Geom::Path> parts;
    for (size_t i = 1; i < kept.size(); ++i) {
        parts.push_back(path.portion(kept[i - 1], kept[i]));
    }
    return parts;
}

/// @a path with @a length of it taken off one end, empty if there is less than that to take.
Geom::Path shortened(Geom::Path const &path, bool at_end, double length)
{
    int const steps = 128;
    double const last = path.size();
    std::vector<std::pair<double, double>> walk{{0.0, 0.0}}; // path time against distance walked
    auto previous = path.initialPoint();
    for (int i = 1; i <= steps; ++i) {
        double const time = last * i / steps;
        auto const point = path.pointAt(time);
        walk.emplace_back(time, walk.back().second + Geom::distance(previous, point));
        previous = point;
    }
    double const total = walk.back().second;
    if (total <= length + TINY) {
        return {};
    }

    double const want = at_end ? total - length : length;
    double time = 0;
    for (size_t i = 1; i < walk.size(); ++i) {
        if (walk[i].second >= want) {
            double const grown = walk[i].second - walk[i - 1].second;
            double const part = grown > TINY ? (want - walk[i - 1].second) / grown : 0;
            time = walk[i - 1].first + part * (walk[i].first - walk[i - 1].first);
            break;
        }
    }
    return at_end ? path.portion(0, time) : path.portion(time, last);
}

Geom::Point middle_of(Geom::Path const &path)
{
    return path.pointAt(path.size() / 2.0);
}

/// Does any of @a path run across @a area?
bool runs_across(Geom::PathVector const &path, Geom::PathVector const &area)
{
    for (auto const &sub : path) {
        for (int i = 1; i < SAMPLES; ++i) {
            if (inside(area, sub.pointAt(sub.size() * double(i) / SAMPLES))) {
                return true;
            }
        }
    }
    return false;
}

bool is_fold_line(Tool const &tool)
{
    return tool.key == "valley" || tool.key == "mountain" || tool.key == "smallValley" ||
           tool.key == "smallMountain";
}

bool is_valley(Tool const &tool)
{
    return tool.key == "valley" || tool.key == "smallValley";
}

/// Mirroring a crease turns a mountain into a valley and the other way about; everything else
/// (creases, x-ray lines) keeps its style. Paper turns over: white shows its gray back and vice
/// versa, while the other shades have no counterpart and stay as they are.
Tool const &turned_over(Tool const &tool)
{
    static std::pair<char const *, char const *> const pairs[] = {
        {"valley", "mountain"}, {"smallValley", "smallMountain"}, {"white", "gray"}};
    for (auto const &[one, other] : pairs) {
        if (tool.key == one) {
            if (auto const found = tool_by_key(other)) {
                return *found;
            }
        }
        if (tool.key == other) {
            if (auto const found = tool_by_key(one)) {
                return *found;
            }
        }
    }
    return tool;
}

/// What the fold command is waiting on between the button press and the click that applies it.
struct Fold
{
    SPDesktop *desktop = nullptr;
    SPItem *crease = nullptr;
    Tool const *crease_tool = nullptr;
    SPItem *paper = nullptr;      ///< the topmost paper the crease crawls across
    std::string crease_id, paper_id; ///< to find them again, in case they went away meanwhile
    Geom::Point q1, q2;           ///< the crease's endpoints, in document coordinates
    Geom::Point heading;          ///< unit vector from q1 to q2: the way the ant crawls
    Geom::Point right;            ///< unit normal, to the ant's right
    double reach = 0;             ///< long enough to cross the whole of the paper
    bool flap_is_right = false;   ///< the half the pointer is over, the half that moves
    CanvasItemPtr<CanvasItemBpath> arrow;
};

/// Never destroyed: the preview belongs to a canvas that may already be gone at exit.
Fold &fold()
{
    static auto *state = new Fold();
    return *state;
}

void say(SPDesktop *desktop, MessageType type, char const *text)
{
    if (desktop) {
        desktop->messageStack()->flash(type, text);
    }
}

/// The fold line, long enough to cross anything it is asked about.
Geom::PathVector fold_line(Fold const &f)
{
    Geom::Path line(f.q1 - f.heading * f.reach);
    line.appendNew<Geom::LineSegment>(f.q1 + f.heading * f.reach);
    return Geom::PathVector(line);
}

/// A curved arrow from one corner of the fold's square to the opposite one, bowed to one side.
Geom::PathVector arrow_between(Geom::Point const &from, Geom::Point const &to)
{
    auto const reach = to - from;
    auto const bow = Geom::rot90(reach) * 0.3;
    auto const grip = from + reach * 0.8 + bow;
    Geom::Path curve(from);
    curve.appendNew<Geom::CubicBezier>(from + reach * 0.2 + bow, grip, to);

    auto const facing = Geom::unit_vector(to - grip);
    double const size = std::max(6.0, reach.length() * 0.16);
    auto const barb = Geom::rot90(facing) * size * 0.5;
    Geom::Path head(to - facing * size + barb);
    head.appendNew<Geom::LineSegment>(to);
    head.appendNew<Geom::LineSegment>(to - facing * size - barb);

    Geom::PathVector arrow;
    arrow.push_back(curve);
    arrow.push_back(head);
    return arrow;
}

void draw_arrow(Fold &f)
{
    if (!f.arrow) {
        return;
    }
    // e1 and e2 are the corners of the square whose other diagonal is the crease itself.
    auto const middle = (f.q1 + f.q2) / 2;
    auto const half = f.right * (Geom::distance(f.q1, f.q2) / 2);
    auto const e1 = middle + half; // to the ant's right
    auto const e2 = middle - half;
    f.arrow->set_bpath(arrow_between(f.flap_is_right ? e1 : e2, f.flap_is_right ? e2 : e1) *
                       f.desktop->doc2dt());
}

/// Which half of the paper the pointer is over: the half that would fold over.
bool points_right(Fold const &f, SPDesktop *desktop, Geom::Point const &world)
{
    auto const point = desktop->w2d(world) * desktop->dt2doc();
    return Geom::dot(point - f.q1, f.right) > 0;
}

void collect_shapes(SPObject *parent, std::vector<SPItem *> &out)
{
    for (auto &child : parent->children) {
        if (auto const item = cast<SPItem>(&child); item && is<SPShape>(item)) {
            out.push_back(item);
        }
        collect_shapes(&child, out);
    }
}

/// The topmost paper the crease crawls across, or nullptr if it crosses none.
SPItem *paper_under(SPDocument *doc, SPItem *crease, Geom::PathVector const &crease_path)
{
    std::vector<SPItem *> shapes;
    collect_shapes(doc->getRoot(), shapes);

    SPItem *found = nullptr;
    for (auto const item : shapes) {
        auto const tool = tool_of(item);
        if (item == crease || !tool || !tool->isPaper()) {
            continue;
        }
        auto const path = item_path(item);
        if (!path.empty() && runs_across(crease_path, path) &&
            (!found || sp_item_repr_compare_position_bool(found, item))) {
            found = item;
        }
    }
    return found;
}

/// Where a copy of an object goes once the original is gone, and how to speak its coordinates.
struct Stencil
{
    SPObject *parent = nullptr;           ///< the copies' parent
    SPObject *scope = nullptr;            ///< the step their numbers are counted within
    Inkscape::XML::Node *model = nullptr; ///< the element they copy, kept alive on its own
    Inkscape::XML::Node *after = nullptr; ///< the sibling they follow, null to go first
    Geom::Affine to_user;                 ///< document coordinates into the model's user space
};

Stencil stencil_of(SPItem *item, std::set<SPObject *> const &doomed)
{
    Stencil stencil;
    stencil.parent = item->parent;
    stencil.scope = naming_scope(item);
    stencil.model = item->getRepr()->duplicate(item->document->getReprDoc());
    stencil.to_user = item->i2doc_affine().inverse();
    // Follow a sibling that is still going to be there once the fold has been applied.
    for (auto node = item->getRepr()->prev(); node; node = node->prev()) {
        auto const object = item->document->getObjectByRepr(node);
        if (!object || !doomed.count(object)) {
            stencil.after = node;
            break;
        }
    }
    return stencil;
}

/// Where a new path goes in the drawing order.
enum class Place
{
    OwnSpot,    ///< where the original was
    AbovePaper, ///< drawn on the folded paper, as lines are
    BelowPaper, ///< tucked under the paper that hides it
};

/// A path waiting for its original to be deleted, so that it can take over its name.
struct Emission
{
    Stencil *stencil = nullptr;
    Geom::PathVector path;
    Tool const *tool = nullptr;
    Place place = Place::OwnSpot;
};

/// Add @a emission to the document, under @a hider if it asked to be hidden by it.
Inkscape::XML::Node *emit(SPDocument *doc, Emission const &emission, Inkscape::XML::Node *hider)
{
    auto &stencil = *emission.stencil;
    auto repr = stencil.model->duplicate(doc->getReprDoc());
    repr->setAttribute("d", sp_svg_write_path(emission.path * stencil.to_user));
    // Tool styles are written in px, so their lengths need the model's user units.
    SPCSSAttr *css = sp_repr_css_attr_new();
    sp_repr_css_attr_add_from_string(css, emission.tool->style.c_str());
    if (double const ex = stencil.to_user.descrim(); ex != 0 && ex != 1) {
        sp_css_attr_scale(css, ex);
    }
    sp_repr_css_set(repr, css, "style");
    sp_repr_css_attr_unref(css);
    if (emission.tool->isPaper()) {
        ensure_def(doc, emission.tool->swatch);
    }
    name_in_step(stencil.scope, repr, *emission.tool);

    auto const parent = stencil.parent->getRepr();
    if (emission.place == Place::OwnSpot || !hider || hider->parent() != parent) {
        parent->addChild(repr, stencil.after);
        stencil.after = repr; // anything else from the same original follows it, in order
    } else {
        parent->addChild(repr, emission.place == Place::AbovePaper ? hider : hider->prev());
    }
    Inkscape::GC::release(repr);
    return repr;
}

/// What one line drawn on the folded paper turns into.
struct LinePlan
{
    SPItem *item = nullptr;
    std::vector<Emission> pieces;
};

/// Plan what becomes of the lines drawn on the paper being folded: the half on the flap is
/// mirrored with it, a line crossing the fold line is split in two, and whatever ends up under
/// the paper on top is dropped, pulled back from it, or pushed below it.
void plan_lines(Fold const &f, bool flap_is_right, Geom::Affine const &mirror,
                Geom::PathVector const &paper_path, Geom::PathVector const &hider,
                bool flap_is_hidden, std::vector<LinePlan> &plans)
{
    std::vector<SPItem *> shapes;
    collect_shapes(f.desktop->getDocument()->getRoot(), shapes);

    for (auto const item : shapes) {
        auto const tool = tool_of(item);
        if (item == f.crease || item == f.paper || !tool || tool->category != "lines") {
            continue;
        }
        auto const path = item_path(item);
        if (path.empty() || !runs_across(path, paper_path)) {
            continue;
        }

        std::vector<Emission> planned;
        bool changed = false;
        for (auto const &sub : path) {
            auto const halves = divide(sub, fold_line(f));
            changed = changed || halves.size() > 1;
            for (auto const &half : halves) {
                bool const on_flap = (Geom::dot(middle_of(half) - f.q1, f.right) > 0) == flap_is_right;
                Emission piece;
                piece.path = Geom::PathVector(half);
                piece.tool = tool;
                piece.place = Place::AbovePaper;
                if (on_flap) {
                    piece.path *= mirror;
                    piece.tool = &turned_over(*tool);
                    changed = true;
                }
                if (on_flap != flap_is_hidden) {
                    planned.push_back(std::move(piece));
                    continue;
                }

                // Under the other half of the paper now: out of sight, in the way, or crossing
                // it often enough that the drawing reads better with the line simply beneath it.
                auto const parts = divide(piece.path.front(), hider);
                if (parts.size() == 1) {
                    if (inside(hider, middle_of(parts.front()))) {
                        changed = true; // nothing of it shows
                        continue;
                    }
                } else if (parts.size() == 2) {
                    // One end of it is covered: drop that end and back the rest away from it.
                    bool const first_hidden = inside(hider, middle_of(parts.front()));
                    auto const kept = shortened(parts[first_hidden ? 1 : 0], !first_hidden, PULL_BACK);
                    changed = true;
                    if (kept.size() == 0) {
                        continue;
                    }
                    piece.path = Geom::PathVector(kept);
                } else {
                    piece.place = Place::BelowPaper;
                    changed = true;
                }
                planned.push_back(std::move(piece));
            }
        }
        if (changed) { // otherwise the fold leaves it alone
            plans.push_back({item, std::move(planned)});
        }
    }
}

/// The stretch of the fold line that crosses the paper: the edge the two halves end up sharing.
std::pair<Geom::Point, Geom::Point> shared_edge(Geom::PathVector const &paper, Fold const &f)
{
    auto const line = fold_line(f);
    std::vector<double> cuts;
    for (auto const &path : paper) {
        for (auto const &crossing : path.intersect(line.front())) {
            cuts.push_back(Geom::dot(crossing.point() - f.q1, f.heading));
        }
    }
    std::sort(cuts.begin(), cuts.end());

    double const crease = Geom::dot((f.q1 + f.q2) / 2 - f.q1, f.heading);
    double best = -1, from = 0, to = 0;
    for (size_t i = 1; i < cuts.size(); ++i) {
        if (cuts[i] - cuts[i - 1] < TINY ||
            !inside(paper, f.q1 + f.heading * ((cuts[i] + cuts[i - 1]) / 2))) {
            continue;
        }
        // The stretch the crease itself lies on wins; failing that, the longest one.
        bool const holds_crease = cuts[i - 1] <= crease && crease <= cuts[i];
        double const score = holds_crease ? 1e9 : cuts[i] - cuts[i - 1];
        if (score > best) {
            best = score;
            from = cuts[i - 1];
            to = cuts[i];
        }
    }
    if (best < 0) {
        return {f.q1, f.q2};
    }
    return {f.q1 + f.heading * from, f.q1 + f.heading * to};
}

/// Notch the paper underneath so that the edge along the fold is drawn once, by the paper on top.
/// The notch is an isosceles triangle on the shared edge with 7 degree base angles; the lines its
/// legs lie on cut the paper into four, and the parts the top hides are left out.
void notch(Geom::PathVector &bottom, Geom::PathVector const &top, Geom::Point const &s1,
           Geom::Point const &s2, Geom::Point const &inward, double reach)
{
    double const half = Geom::distance(s1, s2) / 2;
    if (half < TINY) {
        return;
    }
    auto const apex = (s1 + s2) / 2 + inward * (half * std::tan(NOTCH));
    auto const first = Geom::rot90(Geom::unit_vector(apex - s1));
    auto const second = Geom::rot90(Geom::unit_vector(apex - s2));

    Geom::PathVector kept;
    for (int quarter = 0; quarter < 4; ++quarter) {
        auto piece = combine(bottom, half_plane(apex, (quarter & 1) ? first : -first, reach),
                             bool_op_inters);
        piece = combine(piece, half_plane(apex, (quarter & 2) ? second : -second, reach),
                        bool_op_inters);
        double const size = area(piece);
        if (size > SLIVER && !covered_by(piece, top, size)) {
            kept = combine(kept, piece, bool_op_union);
        }
    }
    bottom = kept;
}

/// Fold the paper over along the crease, the half the user clicked moving onto the other.
void run(Fold &f, bool flap_is_right)
{
    auto const desktop = f.desktop;
    auto doc = desktop->getDocument();
    // The drawing may have moved on since the command was armed; make sure both are still there.
    f.crease = cast<SPItem>(doc->getObjectById(f.crease_id));
    f.paper = cast<SPItem>(doc->getObjectById(f.paper_id));
    if (!f.crease || !f.paper) {
        say(desktop, WARNING_MESSAGE, _("The crease to fold along is gone."));
        return;
    }
    auto const paper_tool = tool_of(f.paper);
    auto const paper_path = item_path(f.paper);
    if (!paper_tool || paper_path.empty()) {
        return;
    }

    auto const outward = flap_is_right ? f.right : -f.right; // fold line towards the moving half
    auto flap = combine(paper_path, half_plane(f.q1, outward, f.reach), bool_op_inters);
    auto rest = combine(paper_path, half_plane(f.q1, -outward, f.reach), bool_op_inters);
    if (area(flap) < SLIVER || area(rest) < SLIVER) {
        say(desktop, WARNING_MESSAGE, _("The fold line does not divide that paper in two."));
        return;
    }
    auto const mirror = Geom::reflection(f.heading, f.q1);
    flap *= mirror;

    // A valley brings the moving half over the top of the other, a mountain tucks it underneath.
    bool const valley = is_valley(*f.crease_tool);
    auto const &top = valley ? flap : rest;
    auto bottom = valley ? rest : flap;
    auto const edge = shared_edge(paper_path, f);
    notch(bottom, top, edge.first, edge.second, -outward, f.reach);

    std::vector<LinePlan> plans;
    plan_lines(f, flap_is_right, mirror, paper_path, top, !valley, plans);

    std::set<SPObject *> doomed{f.crease, f.paper};
    for (auto const &plan : plans) {
        doomed.insert(plan.item);
    }
    std::vector<std::unique_ptr<Stencil>> stencils;
    stencils.push_back(std::make_unique<Stencil>(stencil_of(f.paper, doomed)));
    auto const paper_stencil = stencils.back().get();

    // The paper underneath goes in first, so that the other one lands on top of it. The half
    // that moved shows its other face: white paper turns gray and gray turns white.
    std::vector<Emission> emissions;
    auto const &flap_tool = turned_over(*paper_tool);
    for (bool upper : {false, true}) {
        Emission paper;
        paper.stencil = paper_stencil;
        paper.path = upper ? top : bottom;
        paper.tool = (upper == valley) ? &flap_tool : paper_tool;
        if (area(paper.path) > SLIVER) {
            emissions.push_back(std::move(paper));
        }
    }
    for (auto &plan : plans) {
        stencils.push_back(std::make_unique<Stencil>(stencil_of(plan.item, doomed)));
        for (auto &piece : plan.pieces) {
            piece.stencil = stencils.back().get();
            emissions.push_back(std::move(piece));
        }
    }

    // Only now that every piece has been worked out do the originals go, freeing their names.
    auto const crease_name = f.crease_tool->name;
    for (auto const object : doomed) {
        object->deleteObject(false);
    }

    Inkscape::XML::Node *hider = nullptr;
    for (auto const &emission : emissions) {
        auto const repr = emit(doc, emission, hider);
        if (emission.stencil == paper_stencil) {
            hider = repr; // the last paper added is the one on top, the one lines hide under
        }
    }
    for (auto const &stencil : stencils) {
        Inkscape::GC::release(stencil->model);
    }

    desktop->getSelection()->clear();
    DocumentUndo::done(doc, Glib::ustring::compose(_("Apply %1 fold"), crease_name), "");
}

} // namespace

void apply_fold(SPDesktop *desktop)
{
    cancel_fold();
    if (!desktop) {
        return;
    }
    auto const crease = desktop->getSelection()->singleItem();
    auto const tool = crease ? tool_of(crease) : nullptr;
    if (!tool || !is_fold_line(*tool)) {
        say(desktop, WARNING_MESSAGE, _("Select one mountain or valley crease to fold along."));
        return;
    }
    auto const path = item_path(crease);
    if (path.empty() || Geom::distance(path.front().initialPoint(), path.back().finalPoint()) < TINY) {
        say(desktop, WARNING_MESSAGE, _("That crease has nowhere to fold about."));
        return;
    }
    auto const paper = paper_under(desktop->getDocument(), crease, path);
    if (!paper) {
        say(desktop, WARNING_MESSAGE, _("That crease does not run across any paper."));
        return;
    }

    auto &f = fold();
    f.desktop = desktop;
    f.crease = crease;
    f.crease_tool = tool;
    f.paper = paper;
    f.crease_id = crease->getId() ? crease->getId() : "";
    f.paper_id = paper->getId() ? paper->getId() : "";
    f.q1 = path.front().initialPoint();
    f.q2 = path.back().finalPoint();
    f.heading = Geom::unit_vector(f.q2 - f.q1);
    f.right = right_of(f.heading);
    f.reach = 4 * (span(path) + span(item_path(paper))) + INCH;
    f.flap_is_right = false;

    f.arrow = make_canvasitem<CanvasItemBpath>(desktop->getCanvasControls());
    f.arrow->set_stroke(0x2b7fffff);
    f.arrow->set_fill(0x00000000, SP_WIND_RULE_NONZERO);
    f.arrow->set_stroke_width(2);
    draw_arrow(f);
    say(desktop, NORMAL_MESSAGE, _("Click the half of the paper that folds over, Esc to cancel."));
}

void cancel_fold()
{
    auto &f = fold();
    f.arrow.reset();
    f.desktop = nullptr;
    f.crease = nullptr;
    f.crease_tool = nullptr;
    f.paper = nullptr;
}

bool fold_on_motion(SPDesktop *desktop, Geom::Point const &world)
{
    auto &f = fold();
    if (!f.crease || f.desktop != desktop) {
        return false;
    }
    if (bool const right = points_right(f, desktop, world); right != f.flap_is_right) {
        f.flap_is_right = right;
        draw_arrow(f);
    }
    return false; // the pointer is still the active tool's to use
}

bool fold_on_button_press(SPDesktop *desktop, Geom::Point const &world, unsigned button)
{
    auto &f = fold();
    if (!f.crease || f.desktop != desktop) {
        return false;
    }
    if (button == 3) {
        cancel_fold();
        return true;
    }
    if (button != 1) {
        return false;
    }
    run(f, points_right(f, desktop, world));
    cancel_fold();
    return true;
}

bool fold_on_key_press(SPDesktop *desktop, unsigned keyval)
{
    auto &f = fold();
    if (f.crease && f.desktop == desktop && keyval == GDK_KEY_Escape) {
        cancel_fold();
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
