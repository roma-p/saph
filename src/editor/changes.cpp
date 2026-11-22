// The elements' paths in the lens and the changes to it: selection, removal, duplicates,
// groups, moves, new elements, a lens loaded or inserted (see app.h).

#include <algorithm>
#include <string>
#include <vector>

#include "app.h"

namespace editor {

Json *list_of(App &app, const Path &path) {
    if (path.empty())
        return nullptr;
    const bool shadow = path[0] == -1;
    Json *list = shadow ? &app.lens.shadow() : &app.lens.elements();
    for (size_t i = shadow ? 1 : 0; i + 1 < path.size(); ++i) {
        if (path[i] < 0 || path[i] >= (int)list->size())
            return nullptr;
        Json &e = (*list)[path[i]];
        if (!e.is_object() || !e.contains("children") || !e["children"].is_array())
            return nullptr;
        list = &e["children"];
    }
    return list;
}

Json *element_at(App &app, const Path &path) {
    Json *list = list_of(app, path);
    if (!list || path.size() < (path[0] == -1 ? 2u : 1u) || path.back() < 0 || path.back() >= (int)list->size())
        return nullptr;
    return &(*list)[path.back()];
}

void touch(App &app) {
    app.changed = true;
    app.dirty = true;
}

// Markers put on elements while the tree changes, so that they are found again.
const char *const MARK = "__millsaph_mark";

void strip(Json &list, const char *key) {
    if (!list.is_array())
        return;
    for (Json &e : list)
        if (e.is_object()) {
            e.erase(key);
            if (e.contains("children"))
                strip(e["children"], key);
        }
}

bool find(Json &list, const std::string &mark, Path &path) {
    if (!list.is_array())
        return false;
    for (int i = 0; i < (int)list.size(); ++i) {
        Json &e = list[i];
        if (!e.is_object())
            continue;
        path.push_back(i);
        if (e.value(MARK, std::string()) == mark)
            return true;
        if (e.contains("children") && find(e["children"], mark, path))
            return true;
        path.pop_back();
    }
    return false;
}

// The selection marked "sel" (its order kept), the markers removed after.
void mark_selection(App &app) {
    int k = 0;
    for (const Path &p : app.selected)
        if (Json *e = element_at(app, p))
            (*e)[MARK] = "sel" + std::to_string(k++);
}

void reselect(App &app, int count) {
    app.selected.clear();
    for (int k = 0; k < count; ++k) {
        Path p;
        if (find(app.lens.elements(), "sel" + std::to_string(k), p))
            app.selected.push_back(p);
        else if (Path q{-1}; find(app.lens.shadow(), "sel" + std::to_string(k), q))
            app.selected.push_back(q);
    }
    strip(app.lens.elements(), MARK);
    strip(app.lens.shadow(), MARK);
}

// The marked elements taken out of the tree (in its order).
void take_marked(Json &list, const std::string &prefix, std::vector<Json> &out) {
    if (!list.is_array())
        return;
    for (int i = 0; i < (int)list.size();) {
        Json &e = list[i];
        if (e.is_object() && e.value(MARK, std::string()).rfind(prefix, 0) == 0) {
            out.push_back(e);
            list.erase(list.begin() + i);
            continue;
        }
        if (e.is_object() && e.contains("children"))
            take_marked(e["children"], prefix, out);
        ++i;
    }
}

void remove_selected(App &app) {
    if (app.selected.empty())
        return;
    mark_selection(app);
    std::vector<Json> gone;
    take_marked(app.lens.elements(), "sel", gone);
    take_marked(app.lens.shadow(), "sel", gone);
    app.selected.clear();
    touch(app);
}

void duplicate_selected(App &app) {
    const int n = (int)app.selected.size();
    if (!n)
        return;
    mark_selection(app);
    // each copy after its element, the copies selected
    std::function<void(Json &)> walk = [&](Json &list) {
        for (int i = 0; i < (int)list.size(); ++i) {
            Json &e = list[i];
            if (!e.is_object())
                continue;
            const std::string m = e.value(MARK, std::string());
            if (m.rfind("sel", 0) == 0) {
                Json copy = e;
                e.erase(MARK);
                list.insert(list.begin() + i + 1, copy);
                ++i;
            } else if (e.contains("children")) {
                walk(e["children"]);
            }
        }
    };
    walk(app.lens.elements());
    reselect(app, n);
    touch(app);
}

int count_kind(App &app, const std::string &label) {
    int n = 0;
    std::function<void(const Json &)> walk = [&](const Json &list) {
        for (const Json &e : list)
            if (e.is_object()) {
                if (e.value("name", "").rfind(label + " ", 0) == 0)
                    ++n;
                if (e.contains("children") && e["children"].is_array())
                    walk(e["children"]);
            }
    };
    walk(app.lens.elements());
    return n;
}

void group_selected(App &app) {
    if (app.selected.empty() || app.selected[0][0] == -1)
        return;
    // the group where the first selected element (in the tree's order) was
    std::vector<Path> sel = app.selected;
    std::sort(sel.begin(), sel.end());
    Json *first = element_at(app, sel[0]);
    if (!first)
        return;
    mark_selection(app);
    Json group = lens::new_element("Group", count_kind(app, "Group") + 1);
    group[MARK] = "group";
    Json *list = list_of(app, sel[0]);
    list->insert(list->begin() + sel[0].back(), group);
    std::vector<Json> moved;
    take_marked(app.lens.elements(), "sel", moved);
    Path where;
    if (find(app.lens.elements(), "group", where)) {
        Json *g = element_at(app, where);
        for (Json &m : moved) {
            m.erase(MARK);
            (*g)["children"].push_back(m);
        }
        g->erase(MARK);
        app.selected = {where};
    }
    strip(app.lens.elements(), MARK);
    touch(app);
}

// The selection moved before `target` (into it: at the end of a group's children).
void move_selected(App &app, const Path &target, bool into) {
    for (const Path &p : app.selected)
        if (p[0] == -1 || (target.size() >= p.size() && std::equal(p.begin(), p.end(), target.begin())))
            return; // (not into itself)
    const int n = (int)app.selected.size();
    Json *t = target.empty() ? nullptr : element_at(app, target);
    mark_selection(app);
    if (t)
        (*t)[MARK] = "target";
    std::vector<Json> moved;
    take_marked(app.lens.elements(), "sel", moved);
    Path where;
    Json *list = &app.lens.elements();
    int at = (int)list->size();
    if (t && find(app.lens.elements(), "target", where)) {
        Json *e = element_at(app, where);
        if (into) {
            list = &(*e)["children"];
            at = (int)list->size();
        } else {
            list = list_of(app, where);
            at = where.back();
        }
    }
    for (Json &m : moved)
        list->insert(list->begin() + at++, m);
    reselect(app, n);
    touch(app);
}

void add_element(App &app, const std::string &uitype, const std::string &label) {
    Json e = lens::new_element(uitype, count_kind(app, label) + 1);
    app.lens.elements().push_back(e);
    app.selected = {Path{(int)app.lens.elements().size() - 1}};
    app.scroll_to_selection = true;
    touch(app);
}

void load_lens(App &app, const std::string &text, bool insert) {
    std::string err;
    lens::Lens next;
    if (!lens::parse(text, next, err)) {
        app.status = "Cannot read the lens: " + err;
        return;
    }
    if (insert) {
        for (const Json &e : next.elements())
            app.lens.elements().push_back(e);
    } else {
        app.lens = next;
        app.selected.clear();
    }
    touch(app);
}

} // namespace editor
