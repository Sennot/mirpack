#include "ui_visibility.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    struct Node {
        std::string id;
        bool visible = true;
        bool ownedClass = false;
        bool leaf = false;
        Node* parent = nullptr;
        std::vector<Node*> children;
        Node* objectLayer = nullptr;

        bool isVisible() const { return visible; }
        Node* getParent() const { return parent; }
        void add(Node& child) { children.push_back(&child); child.parent = this; }
    };

    void require(bool condition, char const* message) {
        if (!condition) throw std::runtime_error(message);
    }
}

int main() {
    try {
        using namespace cleanfeed::ui_visibility;
        require(isXdbotID("zilko.xdbot/state-label"), "HUD ownership not detected");
        require(!isXdbotID("zilko.xdbot-copy/state-label") && !isXdbotID("other/zilko.xdbot/button") &&
            !isXdbotID(""), "Unrelated mod matched");

        Node scene{"scene"}, game{"game"}, objects{"objects"};
        Node outgoingGame{"zilko.xdbot/game"}, outgoingObjects{"zilko.xdbot/objects"};
        game.objectLayer = &objects;
        outgoingGame.objectLayer = &outgoingObjects;
        Node hud{"zilko.xdbot/state-label", true, false, true};
        Node invisible{"zilko.xdbot/hidden", false};
        Node mega{"absolllute.hackmega/menu"};
        Node popup{"", true, true}; // xDBot custom popup without an ID.
        Node popupChild{"zilko.xdbot/nested-button"};
        Node hiddenParent{"hidden-parent", false};
        Node hiddenChild{"zilko.xdbot/under-hidden-parent"};
        Node clipped{"generic-geode-scroll", true, false, true};
        Node clippedChild{"zilko.xdbot/setting-control"};
        scene.add(game); game.add(objects); game.add(hud); game.add(invisible);
        scene.add(outgoingGame); outgoingGame.add(outgoingObjects);
        scene.add(mega); scene.add(popup); popup.add(popupChild);
        scene.add(hiddenParent); hiddenParent.add(hiddenChild);
        scene.add(clipped); clipped.add(clippedChild);
        // A large level must have no impact on UI traversal.
        std::vector<Node> levelObjects(100000);
        for (size_t i = 0; i < levelObjects.size(); ++i) {
            (i % 2 ? objects : outgoingObjects).add(levelObjects[i]);
        }

        std::vector<Node*> deferred;
        size_t visited = 0;
        auto gather = [&] {
            std::vector<Node*> excludedContainers;
            auto isContainer = [&](Node* node) {
                return std::find(excludedContainers.begin(), excludedContainers.end(), node) !=
                    excludedContainers.end();
            };
            collect(&scene,
                [&](Node* node) {
                    ++visited;
                    if (isContainer(node)) return false;
                    if (node->objectLayer) {
                        excludedContainers.push_back(node->objectLayer);
                        return false;
                    }
                    return node->ownedClass || isXdbotID(node->id);
                },
                [&](Node* node) { return isContainer(node) || node->leaf; },
                [](Node* node) -> std::vector<Node*> const& { return node->children; },
                [&](Node* node) { deferred.push_back(node); node->visible = false; }
            );
        };
        gather();
        require(deferred.size() == 2 && deferred[0] == &hud && deferred[1] == &popup,
            "UI roots were missed, duplicated or reordered");
        require(visited < 14, "Traversal inspected level geometry or glyph/clip children");
        require(game.visible && objects.visible && outgoingGame.visible && outgoingObjects.visible &&
            mega.visible && popupChild.visible &&
            !invisible.visible && hiddenChild.visible && clippedChild.visible,
            "Unrelated or originally hidden visibility changed");
        require(!visibleUnder(&hud, &scene) && !visibleUnder(&popupChild, &scene),
            "Deferred UI remained visible in clean pass");
        gather();
        require(deferred.size() == 2, "Nested capture queued hidden nodes again");

        // Restore before local rendering or when swapBuffers was skipped.
        for (auto* node : deferred) node->visible = true;
        require(visibleUnder(&hud, &scene) && visibleUnder(&popupChild, &scene),
            "Local UI not restored");
        require(!visibleUnder(&invisible, &scene) && !visibleUnder(&hiddenChild, &scene),
            "Originally hidden nodes became visible");
        popup.parent = nullptr;
        require(!visibleUnder(&popup, &scene), "Detached popup would be drawn after scene exit");
        game.visible = false;
        require(!visibleUnder(&hud, &scene), "HUD ignores a newly hidden ancestor");

        std::cout << "PASS: xDBot-only roots, hidden ancestors, clipped UI, restore, scene exit; "
            "100000 level objects skipped\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
