# MO Chess - rules and data (for anyone re-implementing it)

Mursaat Overseer CM is played normally. On top of it, the addon turns the floor into a chess board
and every player must move like a chess piece. Nothing in the fight is changed.

## Board
- The MO floor is 4x4 big tiles, each 3x3 plates -> **12x12 chess squares** (a plate = a square, 4.68 m).
- Each big tile has a frame band (0.96 m of a 15.95 m tile); standing on a frame counts as the nearest plate.
- Measured corners (map 1188, MumbleLink meters, x y z):
  - c1 start side, right  (square a1): 99.00 106.20 133.10
  - c2 start side, left:                99.03 106.20  69.31
  - c3 far side, left:                  35.27 106.20  69.35
  - c4 far side, right:                 35.17 106.20 133.12
  - Start side = where the squad starts / Jade Soldiers form; far side = where Scouts spawn.
- Files a..l run from the start side to the far side; ranks 1..12 run right to left (seen from the start side).
- Bilinear map between the 4 corners; inverse by a few Newton steps on x/z.

## Pieces
- Claim holder always plays the **Pawn** (moves 1 square toward the far side each move; promotes to Queen at the far edge).
- Everyone else gets a random K, Q, R, B or N when the fight starts (not before).
- Moves are normal chess moves; players can pass through each other, only the end square counts.
- You must move every prompt (staying on your square fails).

## Timing
- Pull -> pieces assigned -> first prompt after 1.5 s, then a prompt every 10 s.
- Each prompt: 5 s to move, then the check. No hints during the move (players must know their piece).
- First prompt also checks the start squares (see below).

## Check (every prompt)
- Fail if: not on a legal end square for your piece, OR another player ends on the same square
  (chess: one piece per square -> both fail).
- Every player's end square shows a mark for 1 s: green ring = good, red X = bad. Then failed players die (/gg).

## Start squares (before the pull)
- 10 squares: a4-a8 and b4-b8 (any player on any of them). Shown gold; yours shimmers when you stand on it.
- Not on a start square at the first prompt = fail.
- After a wipe/reset everyone must return to a start square before the next pull.

## Board on / off
- A gold circle on square a2: stand in it 5 s to switch the board on. It disappears once the board is on.

## Spike patterns
- `mo-spike-patterns.json`: per phase (p1..), the sequence of 4x4 big-tile patterns (X = spikes), one per 10 s.
  Patterns restart at each 25% phase. Rows/cols are as drawn in the community diagrams; the prototype rotated
  them with row = dc, col = 3 - dr to match the board orientation above.

## MO data from Elite Insights (for game-event based versions)
- Species: MO 17172, Jade 17181. Buffs: Claim 37779, Dispel 37697, Protect 37813.
- Special action skills: Claim 38234, Dispel 37904, Protect 38022. Protect bubble = Invulnerability 757 (1 s).
- Phases by boss health 75/50/25. CM if MO max health > 25M.
- EI identifies board tiles with GadgetAnimation events ("areas" = spike tile, "on"/"off" = claim tile).
