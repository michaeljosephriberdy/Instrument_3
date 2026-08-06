Top board orientation
=====================

Layouts top_left.json / top_right.json store the map in PCB/matrix
coordinates with "inverted": false while the boards are upright (same
orientation as the bottom boards). That matches the working setup.

To mount a top board rotated 180°:
  1. Physically rotate the board.
  2. Set "inverted": true in that board's JSON only.
  3. Relaunch (JSON is read at startup; no rebuild required).

Software compensation is a single convertRow/convertColumn in
LayoutManager::actionFor(). There is no second flip elsewhere.

To go back to upright: set "inverted": false again.
