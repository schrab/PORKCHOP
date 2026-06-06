1|// Menu system - Sirloin-style grouped modal
2|#pragma once
3|
4|#include <Arduino.h>
5|6|#include <functional>
7|
8|// Menu item for both root and group items
9|struct MenuItem {
10|    const char* icon;      // 2-char ASCII glyph, "" if none
11|    const char* label;
12|    uint8_t actionId;      // 0 = separator, >0 = action
13|    const char* const* hintPool;
14|    uint8_t hintCount;
15|};
16|
17|// Group identifiers
18|enum class GroupId : int8_t {
19|    NONE = -1,
20|    ATTACK = 0,
21|    RECON = 1,
22|    LOOT = 2,
23|    COMMS = 3,
24|    RANK = 4,
25|    SYSTEM = 5
26|};
27|
28|// Root menu item types
29|enum class RootType : uint8_t {
30|    DIRECT,     // Opens a mode directly
31|    GROUP,      // Opens a modal group
32|    SEPARATOR   // Visual separator, not selectable
33|};
34|
35|struct RootItem {
36|    const char* icon;      // 2-char ASCII glyph, "" if none
37|    const char* label;
38|    const char* const* hintPool;
39|    uint8_t hintCount;
40|    RootType type;
41|    union {
42|        uint8_t actionId;   // For DIRECT type
43|        GroupId groupId;    // For GROUP type
44|    };
45|    const char* hint;
46|};
47|
48|using MenuCallback = std::function<void(uint8_t actionId)>;
49|
50|class Menu {
51|public:
52|    static void init();
53|    static void update();
54|    static void draw(DisplayCanvas& canvas);
55|    
56|    static void setCallback(MenuCallback cb);
57|    
58|    static bool isActive() { return active; }
59|    static bool isInModal() { return activeGroup != GroupId::NONE; }
60|    static bool closeModal();  // Returns true if modal was closed
61|    static void show();
62|    static void hide();
63|    
64|    static const char* getSelectedDescription();  // For bottom bar
65|    
66|private:
67|    // Root menu
68|    static const RootItem ROOT_ITEMS[];
69|    static const uint8_t ROOT_COUNT;
70|    static uint8_t rootIdx;
71|    static uint8_t rootScroll;
72|    
73|    // Group modal
74|    static const MenuItem GROUP_ATTACK[];
75|    static const MenuItem GROUP_RECON[];
76|    static const MenuItem GROUP_LOOT[];
77|    static const MenuItem GROUP_COMMS[];
78|    static const MenuItem GROUP_RANK[];
79|    static const MenuItem GROUP_SYSTEM[];
80|    static const uint8_t GROUP_ATTACK_SIZE;
81|    static const uint8_t GROUP_RECON_SIZE;
82|    static const uint8_t GROUP_LOOT_SIZE;
83|    static const uint8_t GROUP_COMMS_SIZE;
84|    static const uint8_t GROUP_RANK_SIZE;
85|    static const uint8_t GROUP_SYSTEM_SIZE;
86|    
87|    static GroupId activeGroup;
88|    static uint8_t modalIdx;
89|    static uint8_t modalScroll;
90|    
91|    // State
92|    static bool active;
93|    static MenuCallback callback;
94|    static bool keyWasPressed;
95|    
96|    static const uint8_t VISIBLE_ITEMS = 4;
97|    static const uint8_t MODAL_VISIBLE = 4;
98|    static uint8_t rootHintIndex[];
99|    static uint8_t attackHintIndex[];
100|    static uint8_t reconHintIndex[];
101|    static uint8_t lootHintIndex[];
102|    static uint8_t commsHintIndex[];
103|    static uint8_t rankHintIndex[];
104|    static uint8_t systemHintIndex[];
105|    
106|    // Helpers
107|    static void handleInput();
108|    static void drawRoot(DisplayCanvas& canvas);
109|    static void drawModal(DisplayCanvas& canvas);
110|    static bool isRootSelectable(uint8_t idx);
111|    static const MenuItem* getGroupItems(GroupId group);
112|    static uint8_t getGroupSize(GroupId group);
113|    static const char* getGroupName(GroupId group);
114|};
115|