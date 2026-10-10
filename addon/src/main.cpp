// Raid Workshop
// A Nexus addon for player-made challenges on top of Guild Wars 2 raid encounters.
// The fight itself is never changed: the addon only draws on screen and checks your own position.
//
// Encounters:
//   - Mursaat Overseer: MO Chess (board on the floor, a chess piece per player, a move every 10 s)
//   - more later; each encounter gets its own section in the panel and its own data folder.
//
// Uses what Nexus gives every addon (MumbleLink position/camera, ImGui, keybinds, WndProc) and,
// only when switched on in debug mode, an experimental look at the game's depth buffers.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <winhttp.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cfloat>
#include <random>

#include "imgui.h"
#include "Nexus.h"
#include "Mumble.h"

/* ---------------------------------------------------------------------------------------------- */
/* Nexus plumbing                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

static AddonAPI_t*       API      = nullptr;
static Mumble::Data*     ML       = nullptr; // MumbleLink: position + camera
static Mumble::Identity* MI       = nullptr; // parsed identity: FOV, map id
static NexusLinkData_t*  NL       = nullptr; // screen size, gameplay state
static AddonDefinition_t AddonDef = {};

static const char* KB_MARK   = "RAIDWORKSHOP_MARK";
static const char* KB_UNDO   = "RAIDWORKSHOP_UNDO";
static const char* KB_WINDOW = "RAIDWORKSHOP_PANEL";
static const char* KB_DEBUG  = "RAIDWORKSHOP_DEBUG";
static const char* KB_MOVE   = "RAIDWORKSHOP_MO_MOVE";
static const char* LOGCH     = "Raid Workshop";

/* ---------------------------------------------------------------------------------------------- */
/* Small vector math (no DirectX dependency)                                                      */
/* ---------------------------------------------------------------------------------------------- */

struct V3 { float x, y, z; };
static V3    operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3    operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3    operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3    Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Len(V3 a) { return std::sqrt(Dot(a, a)); }
static V3    Norm(V3 a) { float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : V3{ 0, 0, 0 }; }
static V3    FromMumble(const Mumble::Vector3& v) { return { v.X, v.Y, v.Z }; }

// ---- Camera + position for this frame ------------------------------------------------------------
// Read MumbleLink once per frame. The game can be writing it while we read, which gives a torn mix of
// old and new values (the badge jumps for one frame), so we check the tick before and after copying
// and keep the last good copy if they differ. Avatar and camera always come from the same update, so
// overlays stay locked to the character the way the game's own camera is.
static V3    PEye{}, PFront{ 0, 0, 1 }, PAvatar{};
static double NowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
static void UpdateMotion()
{
	uint32_t t0 = ML->UITick;
	V3 eye = FromMumble(ML->CameraPosition), front = FromMumble(ML->CameraFront), av = FromMumble(ML->AvatarPosition);
	uint32_t t1 = ML->UITick;
	if (t0 != t1 || Len(front) < 0.5f) { return; } // torn read: keep last frame's values
	PEye = eye; PFront = front; PAvatar = av;
}

// Project a world point (meters, MumbleLink space) to screen pixels.
// Same math as DirectX XMMatrixLookAtLH + XMMatrixPerspectiveFovLH, which GW2-Compass uses.
// Returns false if the point is behind the camera.
static bool WorldToScreen(V3 p, ImVec2& out)
{
	if (!ML || !MI || !NL || NL->Width == 0 || NL->Height == 0) { return false; }
	V3 eye = PEye;
	V3 zAxis = Norm(PFront);
	V3 xAxis = Norm(Cross(V3{ 0, 1, 0 }, zAxis));
	V3 yAxis = Cross(zAxis, xAxis);
	V3 d = p - eye;
	float vx = Dot(d, xAxis), vy = Dot(d, yAxis), vz = Dot(d, zAxis);
	if (vz < 0.3f) { return false; }
	float fov = MI->FOV > 0.1f ? MI->FOV : 0.873f;
	float t = std::tan(fov * 0.5f);
	float aspect = (float)NL->Width / (float)NL->Height;
	float sx = vx / (vz * t * aspect);
	float sy = vy / (vz * t);
	out = ImVec2((sx + 1.0f) * 0.5f * NL->Width, (1.0f - sy) * 0.5f * NL->Height);
	return true;
}

// Inverse of WorldToScreen: the ray from the camera through a screen pixel, hit against the
// horizontal floor plane y = floorY. Returns false if the ray points away from the floor.
static bool ScreenToFloor(ImVec2 px, float floorY, V3& out)
{
	if (!ML || !MI || !NL || NL->Width == 0 || NL->Height == 0) { return false; }
	V3 eye = PEye;
	V3 zAxis = Norm(PFront);
	V3 xAxis = Norm(Cross(V3{ 0, 1, 0 }, zAxis));
	V3 yAxis = Cross(zAxis, xAxis);
	float fov = MI->FOV > 0.1f ? MI->FOV : 0.873f;
	float t = std::tan(fov * 0.5f);
	float aspect = (float)NL->Width / (float)NL->Height;
	float sx = px.x / NL->Width * 2.0f - 1.0f;
	float sy = 1.0f - px.y / NL->Height * 2.0f;
	V3 dir = zAxis + xAxis * (sx * t * aspect) + yAxis * (sy * t);
	if (std::fabs(dir.y) < 1e-5f) { return false; }
	float k = (floorY - eye.y) / dir.y;
	if (k <= 0 || k > 500) { return false; }
	out = eye + dir * k;
	return true;
}

/* ---------------------------------------------------------------------------------------------- */
/* Calibration state                                                                              */
/* ---------------------------------------------------------------------------------------------- */

// Corner order (board as in the prototype: squad start = files a.., rank 1 = right-hand side when
// you stand on the start side facing the far side where the Scouts spawn).
static const char* CORNER_NAMES[4] = {
	"1. Start side, RIGHT-hand corner (a1)",
	"2. Start side, LEFT-hand corner (a-top)",
	"3. Far side, LEFT-hand corner",
	"4. Far side, RIGHT-hand corner",
};

static std::vector<V3> Corners;           // up to 4
static bool  HasInner = false;            // inner corner of the plates, next to corner 1
static V3    Inner{ 0, 0, 0 };
static float InsetU = 0, InsetV = 0;      // width of a big tile's frame, as a fraction of the tile (0 until measured)
static std::vector<V3> Beams;             // up to 3: the light beams where Claim, Protect and Dispel are picked up (fixed by the arena)
static const char* BEAM_NAMES[3] = { "Claim", "Protect", "Dispel" };
static std::deque<V3>  RecentPos;         // smoothing: MumbleLink position updates ~25x per second
static bool  ShowCalWindow = true;
static bool  ShowGrid   = true;
static bool  ShowSquare = false;
static const int GridN = 12;             // 4x4 big tiles, each made of 3x3 plates
static std::string Status;
static bool  PickMode = false;          // mark with the mouse instead of your feet
static bool  PickValid = false;
static V3    PickPoint{ 0, 0, 0 };

// ---- chess look + practice ----
static bool  ChessLook = true;           // fill the plates as light/dark chess squares
static float ChessAlpha = 0.45f;         // how strong the squares are
static bool  ShowLabels = true;          // a..l / 1..12 along the edges
enum Piece { P_KING, P_QUEEN, P_ROOK, P_BISHOP, P_KNIGHT, P_PAWN, P_COUNT };
static const char* PIECE_NAMES[P_COUNT]  = { "King", "Queen", "Rook", "Bishop", "Knight", "Pawn" };
static const char* PIECE_LETTER[P_COUNT] = { "K", "Q", "R", "B", "N", "P" };
static int   MyPiece = P_KNIGHT;
static bool  ShowHeadIcon = true;
static bool  AutoRepeat = false;         // a new prompt every 10 s
static bool  RealGG = false;             // really type /gg on a fail (it kills you)
static const float MOVE_WINDOW = 5.0f, CYCLE = 10.0f, GG_DELAY = 1.0f, FLASH = 1.8f;
enum Phase { PH_IDLE, PH_MOVING, PH_RESULT };
static Phase PlayPhase = PH_IDLE;
static double PhaseStart = 0, NextAuto = 0;
static int   FromF = 0, FromR = 0;
static bool  Legal[12][12] = {};
static bool  LastPass = false;
static int   ResF = -1, ResR = -1;       // square you ended on, for the reveal
static bool  ResCollide = false;         // failed because someone else ended on your square
// ---- other players ----
// Everyone's addon reports its own position (relay). Debug dummies stand in for other players when
// testing alone. At the check, two players on the same square both fail (chess: one piece per square).
struct Peer { std::string name; V3 pos{ 0, 0, 0 }; double seen = 0; bool dummy = false;
              int resF = -1, resR = -1; bool resPass = true; double resT = 0; };
static std::map<std::string, Peer> Peers;   // key: name ("dummy N" for dummies)
static std::mutex PeersMx;
static const float RESULT_SHOW = 1.0f;       // markers shown this long, then the failed players die
// ---- fight ----
static bool  FightOn = false, TestFight = false, HasPiece = false;
static bool  AutoDetect = true;          // start when you enter combat in the MO room
static bool  IAmClaim = false;           // Claim always plays the Pawn
static bool  BoardMoved = false;         // test board placed at your feet (any map)
static double FightStart = 0, AssignT = 0, OutOfCombatSince = 0;
static bool  PrevCombat = false;
static const float FIRST_PROMPT = 1.5f;  // see your piece, then the first move
static int   Passed = 0, Failed = 0;
static HWND  GameHwnd = nullptr;
struct PendingMsg { double at; UINT msg; WPARAM w; LPARAM l; };
static std::deque<PendingMsg> Outbox;    // keystrokes for /gg, sent a few ms apart
static double Now() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
static void StartMove();
static void QueueGG();
// Start squares: the squad stands on these 10 before the pull (any player on any of them).
static bool IsStartSquare(int f, int r) { return (f == 0 || f == 1) && r >= 3 && r <= 7; } // a4-a8, b4-b8
static bool  FirstMove = false;          // the first move of a fight also checks the start squares
// ---- modes, activation, reset ----
static bool  DebugMode = false;          // debug: calibration window, grid, labels, markers, numbers
static bool  ShowPanel = false;          // the small player panel (ALT+SHIFT+B)
static bool  PieceOnScreen = false;      // show my piece at the bottom of the screen instead of over my head
static bool  BoardActive = false;        // off until someone stands on the activation pad for 5 s
static double ActivatedAt = 0, ResetAt = 0;
static float PadProgress = 0;            // 0..1 while standing on the pad
static bool  HasPad = false; static V3 Pad{ 0, 0, 0 };
static const float PAD_RADIUS = 1.4f, PAD_TIME = 5.0f;
static bool  Armed = false;              // stood on a start square since the last reset: allowed to start a fight
static uint32_t LastMap = 0;
static void SaveSettings();
static std::atomic<int> RelayState{ 0 }; // 0 off, 1 connecting, 2 connected, 3 error
static int PeersOnline();
static const char* DEFAULT_RELAY = "wss://raid-relay.hermangonzalezr.workers.dev";
static char RelayUrl[160] = "";          // empty = DEFAULT_RELAY (override only in debug)
static char RelayRoom[40] = "";          // optional extra code (debug); normally empty
static bool SquadLinkOn = true;          // share my position automatically while the board is on
struct WVert { float x, y, z, w; float r, g, b, a; };   // x,y in NDC; z = distance in front of the camera
static std::vector<WVert> WBuild, WReady;
static bool  InFrame = false;           // the setting
static bool  Capturing = false;         // true while the board is being collected this frame
static int   InjectAt = 2;              // draw when the scene depth buffer is bound for the N-th time
static int   SceneBinds = 0, LastSceneBinds = 0;
static bool  InjectedThisFrame = false;
static int   InjectOk = 0, InjectMiss = 0;
static float InFrameGain = 1.0f;        // color strength inside the frame (the game may brighten/darken it)
static UINT  InjRtvW = 0, InjRtvH = 0, InjRtvFmt = 0, InjRtvN = 0;
static bool  UnderChars = true;          // draw the board under characters (experimental)
static void ReleaseDepthPS();
static void ReleaseInFrame();

static V3 SmoothedPosition()
{
	if (RecentPos.empty()) { return ML ? FromMumble(ML->AvatarPosition) : V3{ 0, 0, 0 }; }
	V3 sum{ 0, 0, 0 };
	for (auto& p : RecentPos) { sum = sum + p; }
	return sum * (1.0f / RecentPos.size());
}

// Board point for (u, v) in [0,1]: u runs from the start side to the far side (files a..),
// v runs from rank 1 to the top rank. Bilinear between the 4 corners, so a slightly uneven
// measurement still lines up.
static V3 BoardPoint(float u, float v)
{
	const V3& c1 = Corners[0]; // u=0, v=0
	const V3& c2 = Corners[1]; // u=0, v=1
	const V3& c3 = Corners[2]; // u=1, v=1
	const V3& c4 = Corners[3]; // u=1, v=0
	return c1 * ((1 - u) * (1 - v)) + c4 * (u * (1 - v)) + c3 * (u * v) + c2 * ((1 - u) * v);
}

// Inverse of BoardPoint on the ground plane (x, z), by a few Newton steps.
static bool BoardUV(V3 p, float& u, float& v)
{
	u = 0.5f; v = 0.5f;
	for (int i = 0; i < 12; i++)
	{
		V3 q = BoardPoint(u, v);
		float e = 1e-3f;
		V3 du = (BoardPoint(u + e, v) - q) * (1.0f / e);
		V3 dv = (BoardPoint(u, v + e) - q) * (1.0f / e);
		float rx = p.x - q.x, rz = p.z - q.z;
		float det = du.x * dv.z - du.z * dv.x;
		if (std::fabs(det) < 1e-9f) { return false; }
		u += (rx * dv.z - rz * dv.x) / det;
		v += (du.x * rz - du.z * rx) / det;
	}
	return true;
}

// Each big tile = a frame band (InsetU/InsetV wide) around 3x3 equal plates.
// A position on the frame counts as the nearest edge plate.
static int PlateIndex(float t, float inset)
{
	int tile = (int)std::floor(t * 4); if (tile < 0) tile = 0; if (tile > 3) tile = 3;
	float local = t * 4 - tile, w = (1 - 2 * inset) / 3;
	int k = local < inset ? 0 : local >= 1 - inset ? 2 : (int)((local - inset) / w);
	if (k > 2) k = 2;
	return tile * 3 + k;
}
static void PlateSpan(int i, float inset, float& a, float& b) // board-space extent of plate i (frame excluded)
{
	int tile = i / 3, k = i % 3; float w = (1 - 2 * inset) / 3;
	a = (tile + inset + k * w) / 4; b = a + w / 4;
}
static void ComputeInset()
{
	float u, v;
	if (HasInner && Corners.size() == 4 && BoardUV(Inner, u, v))
	{
		InsetU = u * 4; InsetV = v * 4;
		if (InsetU < 0 || InsetU > 0.3f) InsetU = 0;
		if (InsetV < 0 || InsetV > 0.3f) InsetV = 0;
	}
	else { InsetU = InsetV = 0; }
}

static std::string SquareName(int file, int rank) // 0-based
{
	const char* files = "abcdefghijkl";
	return std::string(1, files[file]) + std::to_string(rank + 1);
}

static std::string CalibrationPath()
{
	const char* dir = API->Paths_GetAddonDirectory("RaidWorkshop/MO");
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	return std::string(dir) + "/calibration.txt";
}

static bool BoardMovedFlag();
static void Save()
{
	if (BoardMovedFlag()) { Status = "Test board is not saved. Press Back to the MO board first."; return; }
	std::ofstream f(CalibrationPath());
	if (!f) { Status = "Could not write calibration.txt."; return; }
	f << "# MO Chess calibration. Positions in meters (MumbleLink).\n";
	f << "map " << (MI ? MI->MapID : 0) << "\n";
	for (size_t i = 0; i < Corners.size(); i++)
	{
		f << "corner " << i + 1 << " " << Corners[i].x << " " << Corners[i].y << " " << Corners[i].z << "\n";
	}
	if (HasInner) { f << "inner " << Inner.x << " " << Inner.y << " " << Inner.z << "\n"; }
	for (size_t i = 0; i < Beams.size(); i++)
	{
		f << "beam " << BEAM_NAMES[i] << " " << Beams[i].x << " " << Beams[i].y << " " << Beams[i].z << "\n";
	}
	if (HasPad) { f << "pad " << Pad.x << " " << Pad.y << " " << Pad.z << "\n"; }
	Status = "Saved to " + CalibrationPath();
	API->Log(LOGL_INFO, LOGCH, Status.c_str());
}

static const uint32_t MO_MAP = 1188;     // Bastion of the Penitent

// The MO board as measured in game on 2026-10-08. Used when there is no calibration.txt,
// so squadmates get the board without calibrating.
static void LoadBuiltIn()
{
	Corners = { V3{ 99.00f, 106.20f, 133.10f }, V3{ 99.03f, 106.20f, 69.31f },
	            V3{ 35.27f, 106.20f, 69.35f },  V3{ 35.17f, 106.20f, 133.12f } };
	Beams.clear(); HasPad = false;
	InsetU = InsetV = 0.0602f;               // frame 0.96 m of a 15.95 m tile
	Inner = BoardPoint(InsetU / 4, InsetV / 4); HasInner = true;
	Status = "Using the built-in MO board.";
}

static void Load()
{
	std::ifstream f(CalibrationPath());
	if (!f) { f.clear(); f.open(std::string(API->Paths_GetAddonDirectory("MOChess")) + "/calibration.txt"); } // older MO Chess addon
	if (!f) { LoadBuiltIn(); return; }
	std::vector<V3> loaded, loadedBeams;
	HasPad = false;
	HasInner = false;
	std::string line;
	while (std::getline(f, line))
	{
		std::istringstream ss(line);
		std::string key; int idx; V3 p;
		if (!(ss >> key)) { continue; }
		if (key == "corner" && ss >> idx >> p.x >> p.y >> p.z) { loaded.push_back(p); }
		else if (key == "inner" && ss >> p.x >> p.y >> p.z) { Inner = p; HasInner = true; }
		else if (key == "beam") { std::string role; if (ss >> role >> p.x >> p.y >> p.z) { loadedBeams.push_back(p); } }
		else if (key == "pad" && ss >> p.x >> p.y >> p.z) { Pad = p; HasPad = true; }
	}
	if (loaded.size() == 4) { Corners = loaded; Beams = loadedBeams; if (Beams.size() > 3) Beams.resize(3); Status = "Loaded the saved calibration."; ComputeInset(); }
	else { Status = "calibration.txt is incomplete."; }
}

static std::string SettingsPath()
{
	const char* dir = API->Paths_GetAddonDirectory("RaidWorkshop/MO");
	std::error_code ec; std::filesystem::create_directories(dir, ec);
	return std::string(dir) + "/settings.txt";
}
static void SaveSettings()
{
	std::ofstream f(SettingsPath());
	if (!f) { return; }
	f << "debug " << DebugMode << "\nalpha " << ChessAlpha << "\ngg " << RealGG << "\nonscreen " << PieceOnScreen << "\nclaim " << IAmClaim << "\nunder " << UnderChars << "\ninframe " << InFrame << "\ninject " << InjectAt << "\n";
	f << "relayurl " << RelayUrl << "\nrelayroom " << RelayRoom << "\nsquadlink " << SquadLinkOn << "\n";
}
static void LoadSettings()
{
	std::ifstream f(SettingsPath());
	std::string line;
	while (std::getline(f, line))
	{
		std::istringstream ls(line); std::string k; ls >> k;
		if (k == "relayurl" || k == "relayroom")
		{
			std::string val; std::getline(ls, val); size_t a = val.find_first_not_of(' '); val = a == std::string::npos ? "" : val.substr(a);
			if (k == "relayurl") snprintf(RelayUrl, sizeof(RelayUrl), "%s", val.c_str()); else snprintf(RelayRoom, sizeof(RelayRoom), "%s", val.c_str());
			continue;
		}
		float v = 0; if (!(ls >> v)) continue;
		if (k == "debug") DebugMode = v != 0;
		else if (k == "alpha") ChessAlpha = v;
		else if (k == "gg") RealGG = v != 0;
		else if (k == "onscreen") PieceOnScreen = v != 0;
		else if (k == "claim") IAmClaim = v != 0;
		else if (k == "under") UnderChars = v != 0;
		else if (k == "inframe") InFrame = v != 0;
		else if (k == "inject") InjectAt = (int)v;
		else if (k == "squadlink") SquadLinkOn = v != 0;
	}
}

static void MarkCorner()
{
	if (!ML) { return; }
	V3 p = SmoothedPosition();
	if (PickMode)
	{
		if (!PickValid) { Status = "Mouse mode: point the mouse at the floor first."; return; }
		p = PickPoint;
	}
	if (Corners.size() >= 4 && !HasInner)
	{
		Inner = p; HasInner = true; ComputeInset(); Save();
		char b2[160]; snprintf(b2, sizeof(b2), "Marked the inner plate corner. Frame is %.0f%% / %.0f%% of a tile.", InsetU * 100, InsetV * 100);
		Status = b2; API->Log(LOGL_INFO, LOGCH, b2);
		return;
	}
	if (Corners.size() >= 4)
	{
		if (Beams.size() >= 3) { Status = "Corners and beams are all marked. Undo or Clear to redo one."; return; }
		Beams.push_back(p); Save();
		Status = std::string("Marked the ") + BEAM_NAMES[Beams.size() - 1] + " beam. Saved.";
		API->Log(LOGL_INFO, LOGCH, Status.c_str());
		return;
	}
	Corners.push_back(p);
	if (Corners.size() == 4) { Save(); }
	char buf[160];
	snprintf(buf, sizeof(buf), "Marked corner %zu at %.2f, %.2f, %.2f", Corners.size(), p.x, p.y, p.z);
	Status = buf;
	API->Log(LOGL_INFO, LOGCH, buf);
}

static void ProcessKeybind(const char* aIdentifier, bool aIsRelease)
{
	if (aIsRelease) { return; }
	std::string id = aIdentifier;
	if (id == KB_MARK) { MarkCorner(); }
	else if (id == KB_UNDO)
	{
		if (!Beams.empty()) { Beams.pop_back(); Status = "Removed the last beam."; }
		else if (HasInner) { HasInner = false; ComputeInset(); Status = "Removed the inner corner."; }
		else if (!Corners.empty()) { Corners.pop_back(); Status = "Removed the last corner."; }
	}
	else if (id == KB_WINDOW) { ShowPanel = !ShowPanel; }
	else if (id == KB_DEBUG) { DebugMode = !DebugMode; SaveSettings(); }
	else if (id == KB_MOVE) { StartMove(); }
}

/* ---------------------------------------------------------------------------------------------- */
/* Drawing on the floor                                                                           */
/* ---------------------------------------------------------------------------------------------- */

// Draw a board-space line as many short segments, so it follows the floor and clips near the camera.
static void BoardLine(ImDrawList* dl, float u0, float v0, float u1, float v1, ImU32 col, float thick)
{
	const int STEPS = 32;
	ImVec2 prev; bool havePrev = false;
	for (int i = 0; i <= STEPS; i++)
	{
		float t = (float)i / STEPS;
		ImVec2 s;
		bool ok = WorldToScreen(BoardPoint(u0 + (u1 - u0) * t, v0 + (v1 - v0) * t), s);
		if (ok && havePrev) { dl->AddLine(prev, s, col, thick); }
		prev = s; havePrev = ok;
	}
}

// A ring + crosshair on the floor exactly where MumbleLink puts your feet, and dots on every point
// already marked, so you can line up a corner precisely.
static void WorldRing(ImDrawList* dl, V3 c, float radius, ImU32 col, float thick)
{
	const int SEG = 40;
	ImVec2 prev; bool havePrev = false;
	for (int i = 0; i <= SEG; i++)
	{
		float a = 6.2831853f * i / SEG;
		ImVec2 s;
		bool ok = WorldToScreen(V3{ c.x + radius * std::cos(a), c.y, c.z + radius * std::sin(a) }, s);
		if (ok && havePrev) { dl->AddLine(prev, s, col, thick); }
		prev = s; havePrev = ok;
	}
}

static void WorldSegment(ImDrawList* dl, V3 a, V3 b, ImU32 col, float thick)
{
	ImVec2 sa, sb;
	if (WorldToScreen(a, sa) && WorldToScreen(b, sb)) { dl->AddLine(sa, sb, col, thick); }
}

// Fill one chess square (plate), split into 3x3 pieces so it still draws when the camera is close.
static void SoftSquare(ImDrawList* dl, int f, int r, int R, int G, int B, float alpha, bool hole = true);
static void DepthBegin(ImDrawList* dl);
static void DepthEnd(ImDrawList* dl);
// Fill one chess square with a flat color (debug look and highlights). Same drawing as the normal
// look, so it also goes under characters when that is switched on, and has no seams inside.
static void BoardQuad(ImDrawList* dl, int f, int r, ImU32 col)
{
	DepthBegin(dl);
	SoftSquare(dl, f, r, (int)(col & 0xFF), (int)((col >> 8) & 0xFF), (int)((col >> 16) & 0xFF), ((col >> 24) & 0xFF) / 255.0f, false);
	DepthEnd(dl);
}

static void SoftRect(ImDrawList* dl, float u0, float u1, float v0, float v1, int R, int G, int B, float alpha, bool hole);
static bool DepthActive();
static void DepthBegin(ImDrawList* dl);
static void DepthEnd(ImDrawList* dl);
// Square border. With depth on it is drawn as four thin strips lying on the floor (inside the square),
// so characters standing on it cover it like they cover the board. Otherwise plain screen lines.
static void SquareOutline(ImDrawList* dl, int f, int r, ImU32 col, float thick)
{
	float u0, u1, v0, v1;
	PlateSpan(f, InsetU, u0, u1); PlateSpan(r, InsetV, v0, v1);
	if ((DepthActive() || Capturing) && Corners.size() == 4)
	{
		float side = Len(Corners[3] - Corners[0]);               // board side in meters
		float w = (0.025f * thick) / (side > 1 ? side : 1);       // strip width in board units
		int R = col & 0xFF, G = (col >> 8) & 0xFF, B = (col >> 16) & 0xFF; float a = ((col >> 24) & 0xFF) / 255.0f;
		DepthBegin(dl);
		SoftRect(dl, u0, u1, v0, v0 + w, R, G, B, a, false); SoftRect(dl, u0, u1, v1 - w, v1, R, G, B, a, false);
		SoftRect(dl, u0, u0 + w, v0 + w, v1 - w, R, G, B, a, false); SoftRect(dl, u1 - w, u1, v0 + w, v1 - w, R, G, B, a, false);
		DepthEnd(dl);
		return;
	}
	BoardLine(dl, u0, v0, u1, v0, col, thick); BoardLine(dl, u1, v0, u1, v1, col, thick);
	BoardLine(dl, u1, v1, u0, v1, col, thick); BoardLine(dl, u0, v1, u0, v0, col, thick);
}

// ---- polished drawing ------------------------------------------------------------------------------
// A quad with its own color per corner (lets alpha fade smoothly across it).
// Drawing under characters: the board's fill is drawn with our own pixel shader that compares the
// board's distance with the game's depth buffer and skips pixels where something is in front.
static bool DepthActive();
static void DepthBegin(ImDrawList* dl);
static void DepthEnd(ImDrawList* dl);

// ---- Board inside the game's frame (experimental) ----
// Instead of painting over the finished picture, the board's triangles are collected here at the end
// of a frame and drawn by us INSIDE the next frame, right when the game binds its scene depth buffer
// for the chosen time (after the world, before effects and the interface). The GPU's own depth test
// then hides the board behind characters, and telegraphs, name plates and the commander tag are drawn
// by the game on top of it.

// A quad with its own color per corner (lets alpha fade smoothly across it). invz = 1 / distance in
// front of the camera for each corner; used only by the depth shader (passed in the texture coords).
static void GradQuad(ImDrawList* dl, const ImVec2* p, const ImU32* c, const float* invz = nullptr)
{
	if (Capturing && invz && NL && NL->Width && NL->Height)
	{
		static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
		for (int k : tri)
		{
			WVert v;
			v.x = p[k].x / NL->Width * 2.0f - 1.0f; v.y = 1.0f - p[k].y / NL->Height * 2.0f;
			v.z = invz[k] > 1e-6f ? 1.0f / invz[k] : 1e6f; v.w = 1.0f;
			v.r = (c[k] & 0xFF) / 255.0f; v.g = ((c[k] >> 8) & 0xFF) / 255.0f; v.b = ((c[k] >> 16) & 0xFF) / 255.0f; v.a = ((c[k] >> 24) & 0xFF) / 255.0f;
			WBuild.push_back(v);
		}
		return;
	}
	ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	dl->PrimReserve(6, 4);
	ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
	dl->PrimWriteIdx(i0); dl->PrimWriteIdx((ImDrawIdx)(i0 + 1)); dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
	dl->PrimWriteIdx(i0); dl->PrimWriteIdx((ImDrawIdx)(i0 + 2)); dl->PrimWriteIdx((ImDrawIdx)(i0 + 3));
	for (int k = 0; k < 4; k++) { dl->PrimWriteVtx(p[k], invz ? ImVec2(invz[k], 0) : uv, c[k]); }
}
static float Smooth(float e0, float e1, float x) { float t = (x - e0) / (e1 - e0); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }
// 0 at your feet, 1 from about 2 m away: the board fades out around your character so it stays visible.
static float HoleFactor(V3 w)
{
	if (DepthActive() || Capturing) return 1.0f; // characters already show on top: no hole needed
	float dx = w.x - PAvatar.x, dz = w.z - PAvatar.z;
	return Smooth(0.8f, 2.0f, std::sqrt(dx * dx + dz * dz));
}
// A filled rectangle on the board (board coordinates), soft-edged per corner, under characters when
// depth is on. Squares, borders and highlights all use it.
static void SoftRect(ImDrawList* dl, float u0, float u1, float v0, float v1, int R, int G, int B, float alpha, bool hole)
{
	if (Capturing)
	{
		// decal mode: collect the rectangle in board coordinates (u, v); it is painted into the board
		// image and projected onto the real floor inside the game's frame
		if (alpha <= 0.001f) return;
		float uu[6] = { u0, u1, u1, u0, u1, u0 }, vv[6] = { v0, v0, v1, v0, v1, v1 };
		for (int k = 0; k < 6; k++) WBuild.push_back({ uu[k], vv[k], 0, 1, R / 255.0f, G / 255.0f, B / 255.0f, alpha });
		return;
	}
	const int N = 4;
	V3 w[N + 1][N + 1]; ImVec2 sp[N + 1][N + 1]; bool ok[N + 1][N + 1]; float a[N + 1][N + 1]; float iz[N + 1][N + 1];
	V3 fwd = Norm(PFront);
	for (int i = 0; i <= N; i++)
		for (int j = 0; j <= N; j++)
		{
			w[i][j] = BoardPoint(u0 + (u1 - u0) * i / N, v0 + (v1 - v0) * j / N);
			ok[i][j] = WorldToScreen(w[i][j], sp[i][j]);
			a[i][j] = hole ? alpha * HoleFactor(w[i][j]) : alpha;
			float z = Dot(w[i][j] - PEye, fwd); iz[i][j] = z > 0.01f ? 1.0f / z : 100.0f;
		}
	for (int i = 0; i < N; i++)
		for (int j = 0; j < N; j++)
		{
			if (!ok[i][j] || !ok[i + 1][j] || !ok[i + 1][j + 1] || !ok[i][j + 1]) continue;
			if (a[i][j] + a[i + 1][j] + a[i + 1][j + 1] + a[i][j + 1] < 0.01f) continue;
			ImVec2 p[4] = { sp[i][j], sp[i + 1][j], sp[i + 1][j + 1], sp[i][j + 1] };
			ImU32 c[4] = { IM_COL32(R, G, B, (int)(255 * a[i][j])), IM_COL32(R, G, B, (int)(255 * a[i + 1][j])),
			               IM_COL32(R, G, B, (int)(255 * a[i + 1][j + 1])), IM_COL32(R, G, B, (int)(255 * a[i][j + 1])) };
			float zz[4] = { iz[i][j], iz[i + 1][j], iz[i + 1][j + 1], iz[i][j + 1] };
			GradQuad(dl, p, c, (DepthActive() || Capturing) ? zz : nullptr);
		}
}
// Any 4-corner shape lying on the board (board coordinates). Same paths as SoftRect: into the board
// image when drawing inside the game's frame, else on screen with the depth test.
static void BoardQuadUV(ImDrawList* dl, const float* uu, const float* vv, int R, int G, int B, float alpha)
{
	if (alpha <= 0.001f) return;
	if (Capturing)
	{
		static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
		for (int k : tri) WBuild.push_back({ uu[k], vv[k], 0, 1, R / 255.0f, G / 255.0f, B / 255.0f, alpha });
		return;
	}
	ImVec2 p[4]; float iz[4]; V3 fwd = Norm(PFront);
	for (int k = 0; k < 4; k++)
	{
		V3 w = BoardPoint(uu[k], vv[k]);
		if (!WorldToScreen(w, p[k])) return;
		float z = Dot(w - PEye, fwd); iz[k] = z > 0.01f ? 1.0f / z : 100.0f;
	}
	ImU32 c = IM_COL32(R, G, B, (int)(255 * alpha)); ImU32 cc[4] = { c, c, c, c };
	GradQuad(dl, p, cc, DepthActive() ? iz : nullptr);
}
// Result markers inside a square: a green ring (good move) or a red X (bad move).
static void SquareCenter(int f, int r, float& cu, float& cv, float& h)
{
	float u0, u1, v0, v1; PlateSpan(f, InsetU, u0, u1); PlateSpan(r, InsetV, v0, v1);
	cu = (u0 + u1) / 2; cv = (v0 + v1) / 2; h = (u1 - u0) / 2;
}
static void MarkRing(ImDrawList* dl, int f, int r, int R, int G, int B, float alpha, float grow)
{
	float cu, cv, h; SquareCenter(f, r, cu, cv, h);
	float r1 = h * 0.78f * grow, r0 = r1 - h * 0.16f; if (r0 < 0) r0 = 0;
	const int N = 40;
	DepthBegin(dl);
	for (int i = 0; i < N; i++)
	{
		float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
		float uu[4] = { cu + r0 * std::cos(a0), cu + r1 * std::cos(a0), cu + r1 * std::cos(a1), cu + r0 * std::cos(a1) };
		float vv[4] = { cv + r0 * std::sin(a0), cv + r1 * std::sin(a0), cv + r1 * std::sin(a1), cv + r0 * std::sin(a1) };
		BoardQuadUV(dl, uu, vv, R, G, B, alpha);
	}
	DepthEnd(dl);
}
static void MarkX(ImDrawList* dl, int f, int r, int R, int G, int B, float alpha, float grow)
{
	float cu, cv, h; SquareCenter(f, r, cu, cv, h);
	float L = h * 0.72f * grow, T = h * 0.11f;   // half length, half thickness
	DepthBegin(dl);
	for (int k = 0; k < 2; k++)
	{
		float dx = (k == 0 ? 1.0f : -1.0f) * 0.70710678f, dy = 0.70710678f; // diagonal direction
		float nx = -dy, ny = dx;                                             // its normal
		float uu[4] = { cu - dx * L - nx * T, cu + dx * L - nx * T, cu + dx * L + nx * T, cu - dx * L + nx * T };
		float vv[4] = { cv - dy * L - ny * T, cv + dy * L - ny * T, cv + dy * L + ny * T, cv - dy * L + ny * T };
		BoardQuadUV(dl, uu, vv, R, G, B, alpha);
	}
	DepthEnd(dl);
}
// Draw the result for one player: shadow + green ring or red X, popping in.
static void DrawResultMark(ImDrawList* dl, int f, int r, bool pass, float t)
{
	if (f < 0) return;
	float grow = t < 0.15f ? 0.6f + 0.4f * (t / 0.15f) : 1.0f;
	float a = t < RESULT_SHOW ? 1.0f : 1.0f - (t - RESULT_SHOW) / 0.6f; if (a < 0) a = 0;
	if (pass) { MarkRing(dl, f, r, 0, 0, 0, 0.45f * a, grow * 1.06f); MarkRing(dl, f, r, 70, 235, 120, 0.95f * a, grow); }
	else      { MarkX(dl, f, r, 0, 0, 0, 0.45f * a, grow * 1.08f);    MarkX(dl, f, r, 240, 50, 50, 0.95f * a, grow); }
}

static void SoftSquare(ImDrawList* dl, int f, int r, int R, int G, int B, float alpha, bool hole)
{
	float u0, u1, v0, v1;
	PlateSpan(f, InsetU, u0, u1); PlateSpan(r, InsetV, v0, v1);
	SoftRect(dl, u0, u1, v0, v1, R, G, B, alpha, hole);
}
static void WorldArc(ImDrawList* dl, V3 c, float radius, float from, float frac, ImU32 col, float thick)
{
	int seg = (int)(48 * frac) + 2;
	ImVec2 prev; bool havePrev = false;
	for (int i = 0; i <= seg; i++)
	{
		float ang = from + 6.2831853f * frac * i / seg;
		ImVec2 s;
		bool ok = WorldToScreen(V3{ c.x + radius * std::cos(ang), c.y, c.z + radius * std::sin(ang) }, s);
		if (ok && havePrev) { dl->AddLine(prev, s, col, thick); }
		prev = s; havePrev = ok;
	}
}

// The board in the normal (non-debug) look: soft chess squares that fade around you, a thin gold
// edge, and a sweep from the start side when the board switches on. No lines, no letters.
static void DrawBoardPolish()
{
	if (Corners.size() < 4) { return; }
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	float since = (float)(NowSec() - ActivatedAt);
	DepthBegin(dl);
	for (int f = 0; f < 12; f++)
	{
		float sweep = Smooth(0, 1, since * 2.5f - f * 0.15f);
		if (sweep <= 0) continue;
		for (int r = 0; r < 12; r++)
		{
			bool dark = (f + r) % 2 == 0;
			if (dark) SoftSquare(dl, f, r, 22, 19, 17, ChessAlpha * sweep);
			else      SoftSquare(dl, f, r, 240, 232, 214, ChessAlpha * 0.85f * sweep);
		}
	}
	DepthEnd(dl);
	float e = Smooth(0, 1, since * 1.5f - 0.5f);
	if (e > 0)
	{
		ImU32 sh = IM_COL32(0, 0, 0, (int)(110 * e)), gold = IM_COL32(235, 190, 90, (int)(170 * e));
		BoardLine(dl, 0, 0, 1, 0, sh, 5); BoardLine(dl, 1, 0, 1, 1, sh, 5); BoardLine(dl, 1, 1, 0, 1, sh, 5); BoardLine(dl, 0, 1, 0, 0, sh, 5);
		BoardLine(dl, 0, 0, 1, 0, gold, 2); BoardLine(dl, 1, 0, 1, 1, gold, 2); BoardLine(dl, 1, 1, 0, 1, gold, 2); BoardLine(dl, 0, 1, 0, 0, gold, 2);
	}
}

static V3 PadPos()
{
	if (HasPad) { return Pad; }
	float a, b, c, d; PlateSpan(0, InsetU, a, b); PlateSpan(1, InsetV, c, d); // square a2
	return BoardPoint((a + b) / 2, (c + d) / 2);
}

// The activation pad: a gold circle on the floor. Stand in it for 5 s to switch the board on (or off).
static void DrawPad()
{
	if (Corners.size() < 4) { return; }
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	V3 c = PadPos();
	float t = (float)NowSec();
	float pulse = 0.5f + 0.5f * std::sin(t * 2.5f);
	int base = BoardActive ? 90 : 200;
	WorldRing(dl, c, PAD_RADIUS, IM_COL32(0, 0, 0, base * 3 / 4), 7.0f);
	WorldRing(dl, c, PAD_RADIUS, IM_COL32(240, 195, 90, (int)(base * (0.6f + 0.4f * pulse))), 3.0f);
	WorldRing(dl, c, PAD_RADIUS * 0.55f, IM_COL32(240, 195, 90, base / 2), 2.0f);
	// four little marks rotating slowly, so it reads as something to step into
	for (int k = 0; k < 4; k++)
	{
		float ang = t * 0.6f + k * 1.5707963f;
		WorldSegment(dl, V3{ c.x + std::cos(ang) * PAD_RADIUS * 0.7f, c.y, c.z + std::sin(ang) * PAD_RADIUS * 0.7f },
		                 V3{ c.x + std::cos(ang) * PAD_RADIUS * 0.9f, c.y, c.z + std::sin(ang) * PAD_RADIUS * 0.9f }, IM_COL32(240, 195, 90, base), 3.0f);
	}
	if (PadProgress > 0)
	{
		// the filling border: thick and bright so it reads from any distance
		WorldArc(dl, c, PAD_RADIUS, -1.5707963f, PadProgress, IM_COL32(0, 0, 0, 200), 22.0f);
		WorldArc(dl, c, PAD_RADIUS, -1.5707963f, PadProgress, IM_COL32(255, 225, 120, 255), 15.0f);
		WorldArc(dl, c, PAD_RADIUS, -1.5707963f, PadProgress, IM_COL32(255, 250, 220, 255), 5.0f);
	}
}

static bool PadLatch = false; // after a toggle, step out before the pad works again
static void UpdateActivation(float dt)
{
	uint32_t map = MI ? MI->MapID : 0;
	if (map != LastMap)
	{
		if (BoardActive || FightOn) { FightOn = false; HasPiece = false; PlayPhase = PH_IDLE; AutoRepeat = false; NextAuto = 0; }
		BoardActive = false; PadProgress = 0; Armed = false;
		if (BoardMoved) { BoardMoved = false; Load(); }
		LastMap = map;
	}
	bool place = BoardMoved || map == MO_MAP;
	if (!place || Corners.size() < 4 || FightOn || BoardActive) { PadProgress = 0; return; }
	V3 c = PadPos();
	float dx = PAvatar.x - c.x, dz = PAvatar.z - c.z;
	bool in = std::sqrt(dx * dx + dz * dz) < PAD_RADIUS && std::fabs(PAvatar.y - c.y) < 3.0f;
	if (!in) { PadLatch = false; }
	if (in && !PadLatch) { PadProgress += dt / PAD_TIME; }
	else { PadProgress -= dt * 0.8f; if (PadProgress < 0) PadProgress = 0; }
	if (PadProgress >= 1)
	{
		PadProgress = 0; PadLatch = true;
		BoardActive = true; ActivatedAt = NowSec(); Armed = false; Status = "Board on.";
	}
}

static float FloorY()
{
	if (Corners.empty()) { return ML ? ML->AvatarPosition.Y : 0.0f; }
	float y = 0; for (auto& c : Corners) { y += c.y; } return y / Corners.size();
}

static void UpdatePick()
{
	PickValid = false;
	if (!PickMode) { return; }
	ImGuiIO& io = ImGui::GetIO();
	PickValid = ScreenToFloor(io.MousePos, FloorY(), PickPoint);
	// left click on the world (not on the window) marks the point
	if (PickValid && io.MouseClicked[0] && !io.WantCaptureMouse) { MarkCorner(); }
	if (PickValid)
	{
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		const ImU32 cyan = IM_COL32(60, 230, 255, 255), shadow = IM_COL32(0, 0, 0, 200);
		for (int pass = 0; pass < 2; pass++)
		{
			ImU32 col = pass == 0 ? shadow : cyan; float th = pass == 0 ? 5.0f : 2.5f;
			WorldRing(dl, PickPoint, 0.35f, col, th);
			WorldSegment(dl, V3{ PickPoint.x - 0.6f, PickPoint.y, PickPoint.z }, V3{ PickPoint.x + 0.6f, PickPoint.y, PickPoint.z }, col, th);
			WorldSegment(dl, V3{ PickPoint.x, PickPoint.y, PickPoint.z - 0.6f }, V3{ PickPoint.x, PickPoint.y, PickPoint.z + 0.6f }, col, th);
		}
	}
}

static void DrawMarkers()
{
	if (!ML) { return; }
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	V3 f = PAvatar; // follows you smoothly
	const ImU32 shadow = IM_COL32(0, 0, 0, 200), aim = IM_COL32(255, 70, 200, 255);
	for (int pass = 0; pass < 2; pass++)
	{
		ImU32 col = pass == 0 ? shadow : aim;
		float th = pass == 0 ? 5.0f : 2.5f;
		WorldRing(dl, f, 0.35f, col, th);
		WorldSegment(dl, V3{ f.x - 0.6f, f.y, f.z }, V3{ f.x + 0.6f, f.y, f.z }, col, th);
		WorldSegment(dl, V3{ f.x, f.y, f.z - 0.6f }, V3{ f.x, f.y, f.z + 0.6f }, col, th);
	}
	ImVec2 c;
	if (WorldToScreen(f, c)) { dl->AddCircleFilled(c, 4.0f, IM_COL32(255, 255, 255, 255)); }

	// points already marked
	for (size_t i = 0; i < Corners.size(); i++)
	{
		ImVec2 s;
		if (WorldToScreen(Corners[i], s))
		{
			dl->AddCircleFilled(s, 7.0f, IM_COL32(255, 255, 255, 230));
			char t[8]; snprintf(t, sizeof(t), "%zu", i + 1);
			dl->AddText(ImVec2(s.x + 9, s.y - 8), IM_COL32(255, 255, 255, 255), t);
		}
	}
	ImVec2 si;
	if (HasInner && WorldToScreen(Inner, si))
	{
		dl->AddCircleFilled(si, 7.0f, IM_COL32(80, 220, 255, 230));
		dl->AddText(ImVec2(si.x + 9, si.y - 8), IM_COL32(80, 220, 255, 255), "inner");
	}
}

static void DrawBoard()
{
	if (Corners.size() < 4) { return; }
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const ImU32 tileCol   = IM_COL32(20, 20, 20, 230);
	const ImU32 squareCol = IM_COL32(240, 240, 240, 170);
	if (ChessLook)
	{
		int a = (int)(ChessAlpha * 255);
		ImU32 light = IM_COL32(238, 228, 205, a), dark = IM_COL32(25, 22, 20, a);
		for (int f = 0; f < 12; f++)
			for (int r = 0; r < 12; r++)
			{
				ImU32 col = ((f + r) % 2 == 0) ? dark : light; // a1 dark, like a real board
				BoardQuad(dl, f, r, col);
			}
	}
	if (ShowGrid)
	{
		// big tile edges
		for (int i = 0; i <= 4; i++)
		{
			float t = i / 4.0f;
			BoardLine(dl, t, 0, t, 1, tileCol, 4.0f);
			BoardLine(dl, 0, t, 1, t, tileCol, 4.0f);
		}
		// plate edges inside each tile (frame inner edge + the two lines between plates)
		for (int tile = 0; tile < 4; tile++)
			for (int k = 0; k <= 3; k++)
			{
				float wu = (1 - 2 * InsetU) / 3, wv = (1 - 2 * InsetV) / 3;
				float tu = (tile + InsetU + k * wu) / 4, tv = (tile + InsetV + k * wv) / 4;
				BoardLine(dl, tu, 0, tu, 1, squareCol, 1.5f);
				BoardLine(dl, 0, tv, 1, tv, squareCol, 1.5f);
			}
	}

	// the three role beams
	for (size_t i = 0; i < Beams.size(); i++)
	{
		ImVec2 s;
		if (WorldToScreen(Beams[i], s))
		{
			dl->AddCircleFilled(s, 9.0f, IM_COL32(245, 200, 80, 230));
			dl->AddText(ImVec2(s.x + 12, s.y - 8), IM_COL32(245, 200, 80, 255), BEAM_NAMES[i]);
		}
	}

	// file letters along the start side, rank numbers along the right-hand side
	if (ShowLabels)
	{
		for (int i = 0; i < 12; i++)
		{
			float a, b; ImVec2 s;
			PlateSpan(i, InsetU, a, b);
			if (WorldToScreen(BoardPoint((a + b) / 2, -0.015f), s)) { char t[2] = { (char)('a' + i), 0 }; dl->AddText(ImGui::GetFont(), 22, ImVec2(s.x - 6, s.y - 11), IM_COL32(255, 255, 255, 230), t); }
			PlateSpan(i, InsetV, a, b);
			if (WorldToScreen(BoardPoint(-0.015f, (a + b) / 2), s)) { char t[4]; snprintf(t, sizeof(t), "%d", i + 1); dl->AddText(ImGui::GetFont(), 22, ImVec2(s.x - 8, s.y - 11), IM_COL32(255, 255, 255, 230), t); }
		}
	}

	// highlight the square under the player
	float u, v;
	if (ShowSquare && PlayPhase == PH_IDLE && BoardUV(SmoothedPosition(), u, v) && u >= 0 && u < 1 && v >= 0 && v < 1)
	{
		int f = PlateIndex(u, InsetU), r = PlateIndex(v, InsetV);
		float u0, u1, v0, v1;
		PlateSpan(f, InsetU, u0, u1); PlateSpan(r, InsetV, v0, v1);
		BoardQuad(dl, f, r, IM_COL32(60, 200, 140, 90));          // fill goes under characters like the board
		SquareOutline(dl, f, r, IM_COL32(60, 200, 140, 255), 3.0f);
	}
}

/* ---------------------------------------------------------------------------------------------- */
/* Practice: piece, move prompt, check, /gg                                                       */
/* ---------------------------------------------------------------------------------------------- */

static bool SquareOf(V3 p, int& f, int& r)
{
	if (Corners.size() < 4) return false;
	float u, v;
	if (!BoardUV(p, u, v) || u < 0 || u >= 1 || v < 0 || v >= 1) return false;
	f = PlateIndex(u, InsetU); r = PlateIndex(v, InsetV);
	return true;
}
static void RelaySendResult(int f, int r, bool pass);
static bool CurrentSquare(int& f, int& r)
{
	if (!ML || Corners.size() < 4) { return false; }
	float u, v;
	if (!BoardUV(FromMumble(ML->AvatarPosition), u, v) || u < 0 || u >= 1 || v < 0 || v >= 1) { return false; }
	f = PlateIndex(u, InsetU); r = PlateIndex(v, InsetV);
	return true;
}

// Legal end squares from (f, r). Pieces may pass through other players; only the end square counts,
// and staying put is never legal. File = toward the far side, so the Pawn walks +1 file.
static void ComputeLegal(int piece, int f, int r)
{
	for (auto& row : Legal) for (bool& b : row) b = false;
	auto mark = [](int x, int y) { if (x >= 0 && x < 12 && y >= 0 && y < 12) Legal[x][y] = true; };
	auto slide = [&](int dx, int dy) { for (int k = 1; k < 12; k++) mark(f + dx * k, r + dy * k); };
	switch (piece)
	{
	case P_KING:   for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) if (dx || dy) mark(f + dx, r + dy); break;
	case P_ROOK:   slide(1, 0); slide(-1, 0); slide(0, 1); slide(0, -1); break;
	case P_BISHOP: slide(1, 1); slide(1, -1); slide(-1, 1); slide(-1, -1); break;
	case P_QUEEN:  slide(1, 0); slide(-1, 0); slide(0, 1); slide(0, -1); slide(1, 1); slide(1, -1); slide(-1, 1); slide(-1, -1); break;
	case P_KNIGHT: { int d[8][2] = { {1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2} }; for (auto& m : d) mark(f + m[0], r + m[1]); } break;
	case P_PAWN:   mark(f + 1, r); break;
	}
}

static void StartMove()
{
	if (Corners.size() < 4) { Status = "Calibrate the board first."; return; }
	int f, r;
	bool on = CurrentSquare(f, r);
	if (FirstMove)
	{
		// first move of the fight: you must start on one of the 10 start squares
		FirstMove = false;
		if (!on || !IsStartSquare(f, r))
		{
			for (int x = 0; x < 12; x++) for (int y = 0; y < 12; y++) Legal[x][y] = IsStartSquare(x, y);
			LastPass = false; ResF = on ? f : -1; ResR = on ? r : -1;
			Failed++; if (RealGG) { QueueGG(); }
			PlayPhase = PH_RESULT; PhaseStart = Now();
			if (AutoRepeat) { NextAuto = PhaseStart + CYCLE; }
			Status = "Not on a start square when the fight started.";
			return;
		}
	}
	if (!on) { Status = "Stand on the board to get a move."; return; }
	FromF = f; FromR = r;
	ComputeLegal(MyPiece, f, r);
	PlayPhase = PH_MOVING; PhaseStart = Now();
	if (AutoRepeat) { NextAuto = PhaseStart + CYCLE; }
}

static void QueueGG()
{
	if (!GameHwnd || !API) { return; }
	double t = Now() + GG_DELAY;
	auto key = [&](double at, WPARAM vk, UINT scan) {
		Outbox.push_back({ at, WM_KEYDOWN, vk, (LPARAM)(1 | (scan << 16)) });
		Outbox.push_back({ at + 0.02, WM_KEYUP, vk, (LPARAM)(1 | (scan << 16) | (1u << 30) | (1u << 31)) });
	};
	key(t, VK_RETURN, 0x1C);                                  // open chat
	const char* txt = "/gg";
	for (int i = 0; txt[i]; i++) { Outbox.push_back({ t + 0.12 + i * 0.03, WM_CHAR, (WPARAM)txt[i], 1 }); }
	key(t + 0.30, VK_RETURN, 0x1C);                           // send
}

static void BeginFight(bool test)
{
	static std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
	MyPiece = IAmClaim ? P_PAWN : (int)(rng() % 5); // K Q R B N; the Pawn is Claim's
	HasPiece = true; AssignT = Now();
	FightOn = true; TestFight = test; FightStart = AssignT; OutOfCombatSince = 0;
	PlayPhase = PH_IDLE; AutoRepeat = true; NextAuto = AssignT + FIRST_PROMPT;
	Passed = Failed = 0; FirstMove = true;
	Status = test ? "Test fight started." : "Fight detected: piece assigned.";
}

static void EndFight(const char* why)
{
	FightOn = false; TestFight = false; HasPiece = false;
	Armed = false; ResetAt = Now(); FirstMove = false;
	AutoRepeat = false; NextAuto = 0; PlayPhase = PH_IDLE; Outbox.clear();
	Status = why;
}

// Fight start = you enter combat in the MO room while standing on the board (the boss is pulled when
// the third role beam is taken). Fight end = out of combat for 4 s (kill, wipe or reset).
static void DetectFight()
{
	bool combat = ML && ML->Context.IsInCombat;
	bool place = BoardMoved || (MI && MI->MapID == MO_MAP);
	int f, r;
	// arm: standing on a start square while waiting (after a reset everyone has to go back first)
	if (!FightOn && CurrentSquare(f, r) && IsStartSquare(f, r)) { Armed = true; }
	if (AutoDetect && BoardActive && Armed && !FightOn && place && combat && !PrevCombat) { BeginFight(false); }
	if (FightOn && !TestFight)
	{
		if (combat) { OutOfCombatSince = 0; }
		else if (OutOfCombatSince == 0) { OutOfCombatSince = Now(); }
		else if (Now() - OutOfCombatSince > 4.0) { EndFight("Fight over (out of combat)."); }
	}
	PrevCombat = combat;
}

// Test board: the MO board moved so its middle is under your feet, in any map. Not saved.
static bool BoardMovedFlag() { return BoardMoved; }
static void BoardHere()
{
	LoadBuiltIn();
	V3 c = (Corners[0] + Corners[1] + Corners[2] + Corners[3]) * 0.25f;
	V3 me = FromMumble(ML->AvatarPosition);
	V3 d{ me.x - c.x, me.y - c.y, me.z - c.z };
	for (auto& p : Corners) { p = p + d; }
	Inner = Inner + d;
	BoardMoved = true; BoardActive = true; ActivatedAt = NowSec(); Armed = false;
	Status = "Test board placed around you (not saved).";
}

static void UpdatePlay()
{
	DetectFight();
	double now = Now();
	// send queued keystrokes for /gg
	while (!Outbox.empty() && Outbox.front().at <= now)
	{
		PendingMsg m = Outbox.front(); Outbox.pop_front();
		API->WndProc_SendToGameOnly(GameHwnd, m.msg, m.w, m.l);
	}
	if (PlayPhase == PH_MOVING && now - PhaseStart >= MOVE_WINDOW)
	{
		int f, r;
		bool on = CurrentSquare(f, r);
		LastPass = on && Legal[f][r];
		ResF = on ? f : -1; ResR = on ? r : -1;
		ResCollide = false;
		if (on)
		{
			// chess: only one piece per square. Anyone else ending here = both fail.
			std::lock_guard<std::mutex> lk(PeersMx);
			for (auto& kv : Peers)
			{
				const Peer& p = kv.second;
				if (!p.dummy && now - p.seen > 2.0) continue;
				int pf, pr; if (!SquareOf(p.pos, pf, pr)) continue;
				if (pf == f && pr == r) { ResCollide = true; break; }
			}
			if (ResCollide) LastPass = false;
		}
		RelaySendResult(ResF, ResR, LastPass);
		if (LastPass)
		{
			Passed++;
			if (MyPiece == P_PAWN && f == 11) { MyPiece = P_QUEEN; Status = "Pawn reached the end: promoted to Queen!"; }
		}
		else
		{
			Failed++;
			if (RealGG) { QueueGG(); }
		}
		PlayPhase = PH_RESULT; PhaseStart = now;
	}
	if (PlayPhase == PH_RESULT && now - PhaseStart >= FLASH) { PlayPhase = PH_IDLE; }
	if (AutoRepeat && PlayPhase != PH_MOVING && NextAuto > 0 && now >= NextAuto) { StartMove(); }
	if (!AutoRepeat) { NextAuto = 0; }
}

// A badge with the piece letter above your character's head. During a move its gold rim empties
// like a clock; after the check it shows a tick or a cross.
static void DrawHeadIcon(ImDrawList* dl)
{
	if (!ShowHeadIcon || !ML || !HasPiece) { return; }
	ImVec2 s;
	float scale = 1.0f;
	if (PieceOnScreen && NL)
	{
		// fixed spot above the skill bar: never moves, never shakes
		s = ImVec2(NL->Width * 0.5f, NL->Height * 0.80f); scale = 1.4f;
	}
	else
	{
		V3 head = PAvatar + V3{ 0, 3.3f, 0 };   // a bit above the game's own markers
		ImVec2 raw;
		if (!WorldToScreen(head, raw)) { return; }
		// "One Euro" filter: strong smoothing while you hold still or move slowly (kills the shake),
		// almost none when the badge moves fast (so it doesn't trail behind when you turn the camera).
		static ImVec2 x, dx; static double lastT = 0;
		double t = NowSec(); float dt = (float)(t - lastT); lastT = t;
		if (dt <= 0 || dt > 0.25f || std::fabs(raw.x - x.x) > 300 || std::fabs(raw.y - x.y) > 300) { x = raw; dx = ImVec2(0, 0); }
		else
		{
			auto alpha = [](float cutoff, float dt) { float tau = 1.0f / (6.2831853f * cutoff); return 1.0f / (1.0f + tau / dt); };
			ImVec2 v((raw.x - x.x) / dt, (raw.y - x.y) / dt);
			float ad = alpha(1.0f, dt);
			dx = ImVec2(dx.x + (v.x - dx.x) * ad, dx.y + (v.y - dx.y) * ad);
			float speed = std::sqrt(dx.x * dx.x + dx.y * dx.y);
			float a = alpha(0.8f + 0.006f * speed, dt);
			x = ImVec2(x.x + (raw.x - x.x) * a, x.y + (raw.y - x.y) * a);
		}
		s = x;
	}
	float pop = (float)(Now() - AssignT) / 0.6f;   // piece just assigned: badge pops in
	float R = (pop < 1 ? 24 * (1.8f - 0.8f * pop) : 24) * scale;
	const ImU32 gold = IM_COL32(245, 200, 80, 255);
	dl->AddCircleFilled(s, R + 3, IM_COL32(0, 0, 0, 200), 32);
	dl->AddCircleFilled(s, R, IM_COL32(30, 26, 22, 235), 32);
	ImFont* font = ImGui::GetFont();

	if (PlayPhase == PH_RESULT)
	{
		float k = 1.0f - (float)((Now() - PhaseStart) / FLASH); if (k < 0) k = 0;
		ImU32 col = LastPass ? IM_COL32(70, 240, 120, (int)(255 * k)) : IM_COL32(255, 70, 70, (int)(255 * k));
		dl->AddCircle(s, R, col, 32, 4.0f);
		if (LastPass)
		{
			dl->AddLine(ImVec2(s.x - 11, s.y + 1), ImVec2(s.x - 3, s.y + 10), col, 5.0f);
			dl->AddLine(ImVec2(s.x - 3, s.y + 10), ImVec2(s.x + 12, s.y - 9), col, 5.0f);
		}
		else
		{
			dl->AddLine(ImVec2(s.x - 10, s.y - 10), ImVec2(s.x + 10, s.y + 10), col, 5.0f);
			dl->AddLine(ImVec2(s.x + 10, s.y - 10), ImVec2(s.x - 10, s.y + 10), col, 5.0f);
		}
		return;
	}

	if (PlayPhase == PH_MOVING)
	{
		float left = 1.0f - (float)((Now() - PhaseStart) / MOVE_WINDOW); if (left < 0) left = 0;
		dl->AddCircle(s, R, IM_COL32(90, 80, 60, 255), 32, 2.0f);
		const float top = -1.5707963f;
		dl->PathArcTo(s, R, top, top + 6.2831853f * left, 40);
		dl->PathStroke(gold, false, 5.0f);
	}
	else { dl->AddCircle(s, R, gold, 32, 3.0f); }

	float size = 34 * scale;
	ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, PIECE_LETTER[MyPiece]);
	dl->AddText(font, size, ImVec2(s.x - ts.x / 2, s.y - ts.y / 2), gold, PIECE_LETTER[MyPiece]);
	ImVec2 ns = font->CalcTextSizeA(16, FLT_MAX, 0, PIECE_NAMES[MyPiece]);
	dl->AddText(font, 16, ImVec2(s.x - ns.x / 2 + 1, s.y + R + 5), IM_COL32(0, 0, 0, 220), PIECE_NAMES[MyPiece]);
	dl->AddText(font, 16, ImVec2(s.x - ns.x / 2, s.y + R + 4), IM_COL32(255, 255, 255, 255), PIECE_NAMES[MyPiece]);
}

// Soft glow along the screen edges.
static void EdgeGlow(ImDrawList* fg, float W, float H, int r, int g, int b, int alpha, float depth)
{
	ImU32 edge = IM_COL32(r, g, b, alpha), none = IM_COL32(r, g, b, 0);
	float e = H * depth;
	fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, e), edge, edge, none, none);
	fg->AddRectFilledMultiColor(ImVec2(0, H - e), ImVec2(W, H), none, none, edge, edge);
	fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(e, H), edge, none, none, edge);
	fg->AddRectFilledMultiColor(ImVec2(W - e, 0), ImVec2(W, H), none, edge, edge, none);
}

// No hints while moving: you must know how your piece moves. Only the timer is shown.
// When time is up the board reveals every square's border: green = a legal end square, red = not.
static void DrawPlay()
{
	if (Corners.size() < 4 || !NL) { return; }
	ImDrawList* bg = ImGui::GetBackgroundDrawList();
	ImDrawList* fg = ImGui::GetForegroundDrawList();
	float W = (float)NL->Width, H = (float)NL->Height;
	double now = Now();

	if (!FightOn && PlayPhase == PH_IDLE)
	{
		// the 10 start squares in gold. After a reset they flash so everyone goes back.
		// The square you stand on gets a green border (no fill) once you are on it.
		int mf = -1, mr = -1; CurrentSquare(mf, mr);
		float pulse = 0.5f + 0.5f * std::sin((float)now * 3.0f);
		float flash = (float)(now - ResetAt) < 2.0f ? 1.0f - (float)(now - ResetAt) / 2.0f : 0.0f;
		float appear = DebugMode ? 1.0f : Smooth(0, 1, (float)(now - ActivatedAt) * 1.2f - 0.8f);
		for (int f = 0; f < 12; f++)
			for (int r = 0; r < 12; r++)
			{
				if (!IsStartSquare(f, r)) continue;
				bool mine = (f == mf && r == mr);
				// yours: a fast gold shimmer + a bright border that breathes in and out
				float vib = 0.5f + 0.5f * std::sin((float)now * 12.0f);
				float fill = mine ? 0.30f + 0.20f * vib : 0.10f + 0.10f * pulse;
				DepthBegin(bg); SoftSquare(bg, f, r, 255, 200, 70, (fill + 0.35f * flash) * appear); DepthEnd(bg);
				SquareOutline(bg, f, r, IM_COL32(0, 0, 0, (int)(110 * appear)), 5.0f);
				SquareOutline(bg, f, r, IM_COL32(255, mine ? 225 : 200, mine ? 110 : 80, (int)((mine ? 200 + 55 * vib : 180 + 75 * flash) * appear)), mine ? 3.0f + 2.5f * vib : 2.0f);
			}
	}
	if (HasPiece && now - AssignT < 1.0)
	{
		float k = (float)(now - AssignT);
		for (int i = 0; i < 3; i++)
		{
			float q = k - i * 0.15f; if (q < 0 || q > 0.85f) continue;
			WorldRing(bg, PAvatar, 0.3f + q * 4.0f, IM_COL32(255, 220, 120, (int)(255 * (1 - q / 0.85f))), 3.0f);
		}
	}
	if (PlayPhase == PH_MOVING)
	{
		float el = (float)(now - PhaseStart);
		float left = 1.0f - el / MOVE_WINDOW; if (left < 0) left = 0;
		// "move now": a gold pulse spreads from your feet and the screen edges glow for a moment
		if (el < 0.7f)
		{
			float k = el / 0.7f;
			for (int pass = 0; pass < 2; pass++)
				WorldRing(bg, PAvatar, 0.5f + k * 9.0f, pass == 0 ? IM_COL32(0, 0, 0, (int)(120 * (1 - k))) : IM_COL32(255, 200, 70, (int)(255 * (1 - k))), pass == 0 ? 7.0f : 4.0f);
			EdgeGlow(fg, W, H, 255, 190, 60, (int)(150 * (1 - k)), 0.12f);
		}
		// where you started, fading out
		int sa = (int)(220 * left);
		if (sa > 0) { SquareOutline(bg, FromF, FromR, IM_COL32(255, 255, 255, sa), 3.0f); }
		// the timer: a gold ring around your feet that shrinks to nothing
		float rad = 0.4f + 1.6f * left;
		WorldRing(bg, PAvatar, rad, IM_COL32(0, 0, 0, 160), 6.0f);
		WorldRing(bg, PAvatar, rad, left < 0.3f ? IM_COL32(255, 120, 60, 255) : IM_COL32(255, 210, 90, 255), 3.0f);
	}
	else if (PlayPhase == PH_RESULT)
	{
		float k = 1.0f - (float)((now - PhaseStart) / FLASH); if (k < 0) k = 0;
		// a mark in every player's end square: green ring = good move, red X = bad move (or shared square)
		float t = (float)(now - PhaseStart);
		DrawResultMark(bg, ResF, ResR, LastPass, t);
		{
			std::lock_guard<std::mutex> lk(PeersMx);
			for (auto& kv : Peers)
			{
				const Peer& p = kv.second;
				if (p.dummy)
				{
					int pf, pr; if (!SquareOf(p.pos, pf, pr)) continue;
					bool clash = (pf == ResF && pr == ResR);
					DrawResultMark(bg, pf, pr, !clash, t);
				}
				else if (p.resF >= 0 && Now() - p.resT < FLASH)
				{
					DrawResultMark(bg, p.resF, p.resR, p.resPass, (float)(Now() - p.resT));
				}
			}
		}
		EdgeGlow(fg, W, H, LastPass ? 40 : 230, LastPass ? 220 : 30, LastPass ? 90 : 30, (int)((LastPass ? 140 : 190) * k), 0.16f);
	}
	DrawHeadIcon(fg);
}

static void DrawPracticeUI()
{
	if (Corners.size() < 4) { return; }
	ImGui::Separator();
	if (!ImGui::CollapsingHeader("Chess practice", ImGuiTreeNodeFlags_DefaultOpen)) { return; }
	// --- fight ---
	if (FightOn)
	{
		int sec = (int)(Now() - FightStart);
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.7f, 1), "%s running %d:%02d  -  you are the %s", TestFight ? "Test fight" : "Fight", sec / 60, sec % 60, PIECE_NAMES[MyPiece]);
		if (ImGui::Button("Stop fight")) { EndFight("Fight stopped."); }
	}
	else
	{
		ImGui::Text(AutoDetect ? "Waiting for the fight: starts when you enter combat on the board." : "Automatic start is off.");
		if (ImGui::Button("Start test fight")) { BeginFight(true); }
		ImGui::SameLine(); ImGui::TextDisabled("(same as a real pull: piece, then a move every 10 s)");
	}
	ImGui::Checkbox("Start automatically when combat starts", &AutoDetect); ImGui::SameLine();
	ImGui::Checkbox("I hold Claim (Pawn)", &IAmClaim);
	if (ImGui::Button("Test board here (any map)")) { BoardHere(); }
	if (BoardMoved) { ImGui::SameLine(); if (ImGui::Button("Back to the MO board")) { BoardMoved = false; Load(); } }
	ImGui::Separator();
	ImGui::Checkbox("Chessboard look", &ChessLook); ImGui::SameLine();
	ImGui::Checkbox("Letters and numbers", &ShowLabels); ImGui::SameLine();
	ImGui::Checkbox("Grid lines", &ShowGrid);
	ImGui::SetNextItemWidth(160); ImGui::InputFloat("Square strength", &ChessAlpha, 0.01f, 0.1f, "%.2f"); if (ChessAlpha < 0.05f) ChessAlpha = 0.05f; if (ChessAlpha > 1) ChessAlpha = 1;
	ImGui::TextDisabled("Practice without a fight:");
	if (ImGui::Combo("My piece", &MyPiece, PIECE_NAMES, P_COUNT)) { HasPiece = true; AssignT = Now(); }
	ImGui::SameLine();
	if (ImGui::Button("Random"))
	{
		static std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
		MyPiece = (int)(rng() % 5); // K Q R B N; the Pawn is Claim's
		HasPiece = true; AssignT = Now();
	}
	ImGui::Checkbox("Show my piece over my head", &ShowHeadIcon);
	if (ImGui::Button(PlayPhase == PH_MOVING ? "Moving..." : "Move now (ALT+SHIFT+J)")) { if (PlayPhase != PH_MOVING) { HasPiece = true; StartMove(); } }
	ImGui::SameLine(); ImGui::Checkbox("Every 10 s", &AutoRepeat);
	if (AutoRepeat && NextAuto == 0 && PlayPhase == PH_IDLE) { NextAuto = Now() + 3.0; } // first prompt in 3 s
	ImGui::Checkbox("Type /gg in chat when I fail (this kills your character)", &RealGG);
	if (RealGG && !GameHwnd) { ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Game window not found yet: move the mouse over the game once."); }
	ImGui::Text("Passed %d   Failed %d", Passed, Failed); ImGui::SameLine();
	if (ImGui::SmallButton("Reset")) { Passed = Failed = 0; }
}

/* ---------------------------------------------------------------------------------------------- */
/* Depth probe (debug, experimental)                                                              */
/* To draw the board UNDER characters we need the game's depth buffer. This first step only looks */
/* for it: it watches which depth buffers the game binds and clears each frame and reports them.  */
/* It draws nothing. Off by default; turned on from the debug window.                             */
/* ---------------------------------------------------------------------------------------------- */

typedef void (STDMETHODCALLTYPE* OMSetRT_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void (STDMETHODCALLTYPE* ClearDSV_t)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
static void** CtxVT = nullptr;
static OMSetRT_t  OrigOMSetRT = nullptr;
static ClearDSV_t OrigClearDSV = nullptr;
static const int VT_OMSETRT = 33, VT_CLEARDSV = 53; // ID3D11DeviceContext vtable slots
struct DsvStat { ID3D11DepthStencilView* dsv; ID3D11Texture2D* tex; int binds, clears; UINT w, h, fmt, bindFlags, samples; float clearDepth; };
static DsvStat DsvNow[16]; static int DsvNowN = 0;
static DsvStat DsvShown[16]; static int DsvShownN = 0;
static UINT BackW = 0, BackH = 0;
static bool ProbeOn = false;
static ID3D11Device* PDev = nullptr; static ID3D11DeviceContext* PCtx = nullptr;
// Screen-sized depth buffers we keep a handle to: thumbnail (SRV) + CPU read of one pixel (staging copy).
struct DepthCand { ID3D11DepthStencilView* dsv; ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; ID3D11Texture2D* staging; bool pending; float value; int unseen; };
static void DropDepthCopy();
static int CandDropped = 0; // buffers the game stopped using (e.g. recreated after a loading screen)
static DepthCand Cands[6]; static int CandN = 0;
static bool  ReadMouse = false; static double NextRead = 0;
// Auto near plane: twice a second, look at the game's depth on plain board floor around you and work
// out the near plane that makes the game's floor distance match ours. Keeps the test accurate even
// if the game's camera settings differ from what MumbleLink reports.
static bool  AutoNear = true;
static int   DepthBuf = 0;              // which screen-sized buffer holds the scene (A by default)
static float DNear = 0.298f, DFar = 600.0f, DTol = 0.02f;
struct CalSample { int x, y; float z; };
static CalSample CalPts[64]; static int CalN = 0;
static float AutoNearValue = 0; static int AutoNearUsed = 0;
static void AutoNearApply(float n);
static void AddCand(ID3D11DepthStencilView* dsv)
{
	if (CandN >= 6 || !PDev) return;
	ID3D11Resource* res = nullptr; dsv->GetResource(&res); if (!res) return;
	ID3D11Texture2D* tex = nullptr; res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex); res->Release();
	if (!tex) return;
	for (int i = 0; i < CandN; i++) if (Cands[i].tex == tex) { tex->Release(); return; } // same buffer, new view: keep the first
	D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
	DepthCand c{}; c.dsv = dsv; c.tex = tex; c.value = -1;
	if (td.Format == DXGI_FORMAT_R24G8_TYPELESS && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
		PDev->CreateShaderResourceView(tex, &sd, &c.srv);
		D3D11_TEXTURE2D_DESC st = td; st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ; st.MiscFlags = 0;
		PDev->CreateTexture2D(&st, nullptr, &c.staging);
	}
	Cands[CandN++] = c;
}
static void ReleaseCands()
{
	for (int i = 0; i < CandN; i++) { if (Cands[i].srv) Cands[i].srv->Release(); if (Cands[i].staging) Cands[i].staging->Release(); if (Cands[i].tex) Cands[i].tex->Release(); }
	CandN = 0;
}
static std::string ProbeMsg;

static DsvStat* DsvFind(ID3D11DepthStencilView* d)
{
	for (int i = 0; i < DsvNowN; i++) if (DsvNow[i].dsv == d) return &DsvNow[i];
	if (DsvNowN >= 16) return nullptr;
	DsvStat& n = DsvNow[DsvNowN++]; n = {}; n.dsv = d;
	ID3D11Resource* res = nullptr; d->GetResource(&res);
	if (res)
	{
		ID3D11Texture2D* tex = nullptr;
		if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
		{
			D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
			n.w = td.Width; n.h = td.Height; n.fmt = td.Format; n.bindFlags = td.BindFlags; n.samples = td.SampleDesc.Count;
			n.tex = tex; // identity only (no reference kept): valid for this frame
			tex->Release();
		}
		res->Release();
	}
	return &n;
}
static void DrawInFrame(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
static ID3D11DepthStencilView* SceneDsv();
static ID3D11Texture2D* SceneTex();
static bool IsSceneDepth(ID3D11DepthStencilView* dsv)
{
	ID3D11Texture2D* t = SceneTex(); if (!t) return false;
	// The game recreates views (and reuses their addresses), so always compare the buffer itself.
	if (DsvStat* st = DsvFind(dsv)) if (st->tex) return st->tex == t;
	ID3D11Resource* r = nullptr; dsv->GetResource(&r);
	bool res = false;
	if (r) { ID3D11Texture2D* rt = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt); res = (rt == t); if (rt) rt->Release(); r->Release(); }
	return res;
}
static void STDMETHODCALLTYPE HookOMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
{
	if (dsv) { DsvStat* st = DsvFind(dsv); if (st) st->binds++; }
	OrigOMSetRT(c, n, rtv, dsv);
	// The game makes new views of its depth buffer, so compare the buffer itself, not the view.
	if (InFrame && dsv && IsSceneDepth(dsv))
	{
		SceneBinds++;
		if (SceneBinds == InjectAt && !InjectedThisFrame && n > 0 && rtv && rtv[0]) { DrawInFrame(c, n, rtv, dsv); InjectedThisFrame = true; }
	}
}
static void STDMETHODCALLTYPE HookClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
	if (dsv) { DsvStat* st = DsvFind(dsv); if (st) { st->clears++; st->clearDepth = depth; } }
	OrigClearDSV(c, dsv, flags, depth, stencil);
}
static bool PatchSlot(int slot, void* fn, void** orig)
{
	DWORD old;
	if (!VirtualProtect(&CtxVT[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
	if (orig) *orig = CtxVT[slot];
	CtxVT[slot] = fn;
	VirtualProtect(&CtxVT[slot], sizeof(void*), old, &old);
	return true;
}
static void ProbeStart()
{
	if (CtxVT || !API || !API->SwapChain) { ProbeMsg = "No swap chain from Nexus."; return; }
	IDXGISwapChain* sc = (IDXGISwapChain*)API->SwapChain;
	ID3D11Device* dev = nullptr;
	if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) { ProbeMsg = "Could not get the D3D11 device."; return; }
	ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
	if (!ctx) { dev->Release(); ProbeMsg = "Could not get the device context."; return; }
	PDev = dev; PCtx = ctx; ctx->AddRef(); // keep both while the probe runs
	DXGI_SWAP_CHAIN_DESC d; if (SUCCEEDED(sc->GetDesc(&d))) { BackW = d.BufferDesc.Width; BackH = d.BufferDesc.Height; }
	CtxVT = *(void***)ctx; ctx->Release(); // drop the extra ref: PCtx keeps the one from GetImmediateContext
	void* o1 = nullptr; void* o2 = nullptr;
	PatchSlot(VT_OMSETRT, (void*)&HookOMSetRT, &o1); OrigOMSetRT = (OMSetRT_t)o1;
	PatchSlot(VT_CLEARDSV, (void*)&HookClearDSV, &o2); OrigClearDSV = (ClearDSV_t)o2;
	ProbeOn = true; ProbeMsg = "Watching depth buffers.";
}
static void ProbeStop()
{
	if (!CtxVT) { ProbeOn = false; return; }
	// only put back what is still ours (another addon may have patched after us)
	if (CtxVT[VT_OMSETRT] == (void*)&HookOMSetRT) PatchSlot(VT_OMSETRT, (void*)OrigOMSetRT, nullptr);
	if (CtxVT[VT_CLEARDSV] == (void*)&HookClearDSV) PatchSlot(VT_CLEARDSV, (void*)OrigClearDSV, nullptr);
	CtxVT = nullptr; ProbeOn = false; DsvNowN = DsvShownN = 0; ProbeMsg = "Stopped.";
	ReleaseCands();
	ReleaseDepthPS();
	ReleaseInFrame(); WReady.clear(); WBuild.clear();
	if (PCtx) { PCtx->Release(); PCtx = nullptr; }
	if (PDev) { PDev->Release(); PDev = nullptr; }
}
static void ProbeFrame() // once per frame: publish last frame's numbers, start counting again
{
	if (!ProbeOn) return;
	DsvShownN = DsvNowN; for (int i = 0; i < DsvNowN; i++) DsvShown[i] = DsvNow[i];
	LastSceneBinds = SceneBinds; SceneBinds = 0;
	if (InjectedThisFrame) { InjectOk++; InjectMiss = 0; } else { InjectMiss++; }
	InjectedThisFrame = false;
	{ // screen size can change (window resize, fullscreen toggle)
		DXGI_SWAP_CHAIN_DESC d; IDXGISwapChain* sc = API ? (IDXGISwapChain*)API->SwapChain : nullptr;
		if (sc && SUCCEEDED(sc->GetDesc(&d))) { BackW = d.BufferDesc.Width; BackH = d.BufferDesc.Height; }
	}
	// Keep the candidates in step with the buffers the game really uses: mark the ones seen this frame,
	// drop the ones not used for a second (the game recreated them), then add new ones in bind order.
	for (int i = 0; i < CandN; i++) Cands[i].unseen++;
	for (int i = 0; i < DsvNowN; i++)
		for (int j = 0; j < CandN; j++)
			if (DsvNow[i].tex && DsvNow[i].tex == Cands[j].tex) { Cands[j].unseen = 0; Cands[j].dsv = DsvNow[i].dsv; }
	{
		int k = 0; bool dropped = false;
		for (int i = 0; i < CandN; i++)
		{
			DepthCand& c = Cands[i];
			D3D11_TEXTURE2D_DESC td{}; if (c.tex) c.tex->GetDesc(&td);
			if (c.unseen > 60 || td.Width != BackW || td.Height != BackH)
			{
				if (c.srv) c.srv->Release(); if (c.staging) c.staging->Release(); if (c.tex) c.tex->Release();
				dropped = true; continue;
			}
			Cands[k++] = c;
		}
		CandN = k;
		if (dropped) { CandDropped++; DropDepthCopy(); }
	}
	for (int i = 0; i < DsvNowN; i++) if (DsvNow[i].w == BackW && DsvNow[i].h == BackH) AddCand(DsvNow[i].dsv);
	if (DepthBuf >= CandN && CandN > 0) DepthBuf = 0;
	DsvNowN = 0;
	if ((!ReadMouse && !(AutoNear && UnderChars)) || !PCtx) return;
	// copy the buffer(s) to CPU-readable textures, read them a frame later
	ImVec2 m = ImGui::GetIO().MousePos;
	int mx = (int)m.x, my = (int)m.y;
	for (int i = 0; i < CandN; i++)
	{
		DepthCand& c = Cands[i];
		if (!c.staging || !c.pending) continue;
		D3D11_MAPPED_SUBRESOURCE ms;
		HRESULT hr = PCtx->Map(c.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
		if (SUCCEEDED(hr))
		{
			auto px = [&](int x, int y) { uint32_t v = *(uint32_t*)((uint8_t*)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * 4); return (v & 0xFFFFFF) / 16777215.0f; };
			if (mx >= 0 && my >= 0 && (UINT)mx < BackW && (UINT)my < BackH) c.value = px(mx, my);
			if (i == DepthBuf && CalN > 0)
			{
				// near plane each floor sample implies (exact for the game's projection with far plane DFar)
				float ns[64]; int k = 0;
				for (int j = 0; j < CalN; j++)
				{
					float d = px(CalPts[j].x, CalPts[j].y), z = CalPts[j].z;
					if (d <= 0 || d >= 1) continue;
					float n = z * DFar * (1 - d) / (DFar - z * d);
					if (n > 0.05f && n < 2.0f) ns[k++] = n;
				}
				if (k >= 5)
				{
					std::sort(ns, ns + k);
					// low quartile: points hidden by something in front give a too-large value, never too small
					AutoNearValue = ns[k / 4]; AutoNearUsed = k;
					if (AutoNear) AutoNearApply(AutoNearValue);
				}
			}
			PCtx->Unmap(c.staging, 0);
		}
		c.pending = false;
	}
	double t = NowSec();
	if (t >= NextRead && Corners.size() == 4)
	{
		NextRead = t + 0.5;
		// floor sample points for the auto near plane: square centers 4-30 m from the camera, away from you
		CalN = 0;
		V3 fwd = Norm(PFront);
		for (int f = 0; f < 12 && CalN < 64; f++)
			for (int r = 0; r < 12 && CalN < 64; r++)
			{
				float a0, a1, b0, b1; PlateSpan(f, InsetU, a0, a1); PlateSpan(r, InsetV, b0, b1);
				V3 w = BoardPoint((a0 + a1) / 2, (b0 + b1) / 2);
				float dx = w.x - PAvatar.x, dz = w.z - PAvatar.z;
				if (dx * dx + dz * dz < 9.0f) continue;      // not near your own character
				float z = Dot(w - PEye, fwd);
				if (z < 4.0f || z > 30.0f) continue;
				ImVec2 sp; if (!WorldToScreen(w, sp)) continue;
				if (sp.x < 0 || sp.y < 0 || sp.x >= BackW || sp.y >= BackH) continue;
				CalPts[CalN++] = { (int)sp.x, (int)sp.y, z };
			}
		for (int i = 0; i < CandN; i++)
		{
			bool want = ReadMouse || i == DepthBuf;
			if (want && Cands[i].staging && !Cands[i].pending) { PCtx->CopyResource(Cands[i].staging, Cands[i].tex); Cands[i].pending = true; }
		}
	}
}
static void ProbeUI()
{
	ImGui::Separator();
	ImGui::TextDisabled("Depth probe (experimental, step 1 of drawing the board under characters)");
	if (!ProbeOn) { if (ImGui::Button("Start depth probe")) ProbeStart(); }
	else { if (ImGui::Button("Stop depth probe")) ProbeStop(); }
	if (!ProbeMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", ProbeMsg.c_str()); }
	if (!ProbeOn) return;
	ImGui::Text("Screen %u x %u. Depth buffers used last frame: %d", BackW, BackH, DsvShownN);
	for (int i = 0; i < DsvShownN; i++)
	{
		DsvStat& d = DsvShown[i];
		bool screen = d.w == BackW && d.h == BackH;
		ImGui::TextColored(screen ? ImVec4(0.5f, 1, 0.6f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
			"#%d  %u x %u  format %u  samples %u  readable %s  binds %d  clears %d  clear value %.2f",
			i, d.w, d.h, d.fmt, d.samples, (d.bindFlags & D3D11_BIND_SHADER_RESOURCE) ? "yes" : "no", d.binds, d.clears, d.clearDepth);
	}
	ImGui::Separator();
	ImGui::Text("Screen-sized depth buffers: %d", CandN);
	if (CandDropped) { ImGui::SameLine(); ImGui::TextDisabled("(renewed %d time(s) after the game recreated them)", CandDropped); }
	ImGui::Checkbox("Read depth under the mouse (2x per second)", &ReadMouse);
	V3 hit; float vz = -1;
	if (ScreenToFloor(ImGui::GetIO().MousePos, FloorY(), hit)) { vz = Dot(hit - PEye, Norm(PFront)); }
	if (ReadMouse) ImGui::Text("Floor under the mouse is %.1f m in front of the camera", vz);
	for (int i = 0; i < CandN; i++)
	{
		DepthCand& c = Cands[i];
		ImGui::PushID(i);
		ImGui::BeginGroup();
		if (c.srv) ImGui::Image((ImTextureID)c.srv, ImVec2(192, 192.0f * BackH / (BackW ? BackW : 1)));
		ImGui::Text("Buffer %c", 'A' + i);
		if (ReadMouse && c.value >= 0)
		{
			ImGui::Text("value %.6f", c.value);
			if (vz > 0) ImGui::Text("x dist %.3f", c.value * vz); // constant everywhere = reversed depth, this is the near plane
		}
		ImGui::EndGroup();
		ImGui::PopID();
		if (i % 2 == 0 && i + 1 < CandN) ImGui::SameLine();
	}
	ImGui::TextDisabled("Point the mouse at the floor in 3 places (near, middle, far) and screenshot each time.");
}

/* ---------------------------------------------------------------------------------------------- */
/* Board under characters (experimental)                                                          */
/* Measured in game: depth is stored the standard way (1 = far), near plane about 0.3 m.          */
/* ---------------------------------------------------------------------------------------------- */

static ID3D11PixelShader* DepthPS = nullptr;
static float DepthPSBuilt[3] = { 0, 0, 0 };
static std::string DepthErr;
static ID3D11Buffer* DepthCB = nullptr;
static float DBias = 0.15f;
static void ReleaseDepthPS() { if (DepthPS) { DepthPS->Release(); DepthPS = nullptr; } if (DepthCB) { DepthCB->Release(); DepthCB = nullptr; } }

static bool DepthActive()
{
	return !Capturing && UnderChars && ProbeOn && PCtx && DepthBuf < CandN && Cands[DepthBuf].srv && DepthPS && DepthCB;
}
static void BuildDepthShader()
{
	if (!PDev) return;
	if (DepthPS && DepthCB) return;
	typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
	static D3DCompile_t compile = nullptr;
	if (!compile)
	{
		HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
		if (m) compile = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
		if (!compile) { DepthErr = "d3dcompiler_47.dll not found."; return; }
	}
	const char* src =
		"struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"cbuffer P : register(b0) { float A; float B; float tol; float bias; };\n"
		"Texture2D<float> sceneDepth : register(t1);\n"
		"float4 main(PS_INPUT i) : SV_Target {\n"
		"  float d = sceneDepth.Load(int3(i.pos.xy, 0));\n"
		"  float zs = (d >= A) ? 1e9 : B / (A - d);\n"           // the game's distance at this pixel
		"  float zb = 1.0 / max(i.uv.x, 1e-6);\n"                 // our board's distance
		"  if (zs < zb * (1.0 - tol) - bias) discard;\n"          // something stands in front of the floor
		"  return i.col;\n"
		"}\n";
	ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
	HRESULT hr = compile(src, strlen(src), "rw_depth", nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, &err);
	if (FAILED(hr)) { DepthErr = err ? (const char*)err->GetBufferPointer() : "Shader failed to compile."; if (err) err->Release(); return; }
	PDev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &DepthPS);
	code->Release(); if (err) err->Release();
	if (!DepthCB)
	{
		D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		PDev->CreateBuffer(&bd, nullptr, &DepthCB);
	}
	DepthPSBuilt[0] = 1;
	DepthErr.clear();
}
static void CbDepthOn(const ImDrawList*, const ImDrawCmd*)
{
	if (!DepthActive()) return;
	D3D11_MAPPED_SUBRESOURCE ms;
	if (SUCCEEDED(PCtx->Map(DepthCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
	{
		float* p = (float*)ms.pData;
		p[0] = DFar / (DFar - DNear); p[1] = DNear * DFar / (DFar - DNear); p[2] = DTol; p[3] = DBias;
		PCtx->Unmap(DepthCB, 0);
	}
	PCtx->PSSetShader(DepthPS, nullptr, 0);
	PCtx->PSSetConstantBuffers(0, 1, &DepthCB);
	ID3D11ShaderResourceView* srv = Cands[DepthBuf].srv;
	PCtx->PSSetShaderResources(1, 1, &srv);
}
static void CbDepthOff(const ImDrawList*, const ImDrawCmd*)
{
	if (!PCtx) return;
	ID3D11ShaderResourceView* none = nullptr;
	PCtx->PSSetShaderResources(1, 1, &none);
	ID3D11Buffer* nb = nullptr;
	PCtx->PSSetConstantBuffers(0, 1, &nb);
}
static void AutoNearApply(float n) { DNear = DNear * 0.7f + n * 0.3f; } // smooth, so it doesn't jump around
static void DepthBegin(ImDrawList* dl) { if (DepthActive()) dl->AddCallback(CbDepthOn, nullptr); }
static void DepthEnd(ImDrawList* dl)
{
	if (!DepthActive()) return;
	dl->AddCallback(CbDepthOff, nullptr);
	dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr); // back to ImGui's own shader
}
static void DepthFrame() // once per frame
{
	if (UnderChars && !ProbeOn) ProbeStart();
	if (UnderChars && ProbeOn) BuildDepthShader();
}
static ID3D11DepthStencilView* SceneDsv() { return (DepthBuf < CandN) ? Cands[DepthBuf].dsv : nullptr; }
static ID3D11Texture2D* SceneTex() { return (DepthBuf < CandN) ? Cands[DepthBuf].tex : nullptr; }
// Decal drawing, the way the game draws its own ground marks:
//  1. paint this frame's board (squares, borders, highlights) into a 2048x2048 "board image";
//  2. copy the game's depth buffer (after the world, before the game's decals);
//  3. one full-screen pass: for every pixel, rebuild its position in the world from depth, and if it
//     is floor inside the board, paint the board image there. Characters, the boss and props are
//     not floor, so they are never painted; the game's decals, effects and interface come after us.
static ID3D11VertexShader* IFBoardVS = nullptr; static ID3D11PixelShader* IFBoardPS = nullptr; static ID3D11InputLayout* IFLayout = nullptr;
static ID3D11VertexShader* IFDecalVS = nullptr; static ID3D11PixelShader* IFDecalPS = nullptr;
static ID3D11Buffer* IFVB = nullptr; static UINT IFVBCap = 0;
static ID3D11Buffer* IFCB = nullptr;
static ID3D11Texture2D* IFBoardTex = nullptr; static ID3D11RenderTargetView* IFBoardRTV = nullptr; static ID3D11ShaderResourceView* IFBoardSRV = nullptr;
static ID3D11Texture2D* IFDepthCopy = nullptr; static ID3D11ShaderResourceView* IFDepthSRV = nullptr; static UINT IFDepthW = 0, IFDepthH = 0;
static ID3D11SamplerState* IFSampler = nullptr;
static ID3D11BlendState* IFBlendBoard = nullptr; static ID3D11BlendState* IFBlendDecal = nullptr;
static ID3D11DepthStencilState* IFNoDepth = nullptr; static ID3D11RasterizerState* IFRaster = nullptr;
static std::string IFErr;
static float IFHeightTol = 0.30f;       // how far above/below the board height still counts as floor (m)
static const UINT BOARD_TEX = 2048;
template <class T> static void SafeRel(T*& p) { if (p) { p->Release(); p = nullptr; } }
static void DropDepthCopy() { SafeRel(IFDepthSRV); SafeRel(IFDepthCopy); IFDepthW = IFDepthH = 0; }
static void ReleaseInFrame()
{
	SafeRel(IFBoardVS); SafeRel(IFBoardPS); SafeRel(IFLayout); SafeRel(IFDecalVS); SafeRel(IFDecalPS); SafeRel(IFVB); IFVBCap = 0; SafeRel(IFCB);
	SafeRel(IFBoardSRV); SafeRel(IFBoardRTV); SafeRel(IFBoardTex); SafeRel(IFDepthSRV); SafeRel(IFDepthCopy); IFDepthW = IFDepthH = 0;
	SafeRel(IFSampler); SafeRel(IFBlendBoard); SafeRel(IFBlendDecal); SafeRel(IFNoDepth); SafeRel(IFRaster);
}
static bool BuildInFrame()
{
	if (IFBoardVS && IFBoardPS && IFLayout && IFDecalVS && IFDecalPS && IFCB && IFBoardTex && IFSampler && IFBlendBoard && IFBlendDecal && IFNoDepth && IFRaster) return true;
	if (!PDev || !IFErr.empty()) return false;
	typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
	static D3DCompile_t compile = nullptr;
	if (!compile) { HMODULE m = LoadLibraryA("d3dcompiler_47.dll"); if (m) compile = (D3DCompile_t)GetProcAddress(m, "D3DCompile"); }
	if (!compile) { IFErr = "d3dcompiler_47.dll not found."; return false; }
	auto comp = [&](const char* src, const char* target, ID3DBlob** out) -> bool {
		ID3DBlob* err = nullptr;
		HRESULT hr = compile(src, strlen(src), "rw", nullptr, nullptr, "main", target, 0, 0, out, &err);
		if (FAILED(hr)) { IFErr = err ? (const char*)err->GetBufferPointer() : "shader failed"; if (err) err->Release(); return false; }
		if (err) err->Release();
		return true;
	};
	// board image pass: (u, v) -> image, premultiplied color
	const char* bvs =
		"struct VI { float4 pos : POSITION; float4 col : COLOR; };\n"
		"struct VO { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
		"VO main(VI i) { VO o; o.pos = float4(i.pos.x * 2 - 1, i.pos.y * 2 - 1, 0, 1); o.col = float4(i.col.rgb * i.col.a, i.col.a); return o; }\n";
	const char* bps =
		"struct VO { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
		"float4 main(VO i) : SV_Target { return i.col; }\n";
	// decal pass: full screen triangle
	const char* dvs =
		"float4 main(uint id : SV_VertexID) : SV_POSITION { float2 t = float2((id << 1) & 2, id & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n";
	const char* dps =
		"cbuffer P : register(b0) { float4 eye; float4 xa; float4 ya; float4 za; float4 proj; float4 board; float4 inv; float4 sizes; float4 misc; };\n"
		"Texture2D<float> sceneDepth : register(t0);\n"
		"Texture2D boardImg : register(t1);\n"
		"SamplerState smp : register(s0);\n"
		"float4 main(float4 pos : SV_POSITION) : SV_Target {\n"
		"  int2 dp = int2(pos.xy * sizes.zw / sizes.xy);\n"
		"  float d = sceneDepth.Load(int3(dp, 0));\n"
		"  if (d >= proj.z) discard;\n"                                        // sky / nothing drawn
		"  float z = proj.w / (proj.z - d);\n"                                  // distance in front of the camera
		"  float2 ndc = float2(pos.x / sizes.x * 2 - 1, 1 - pos.y / sizes.y * 2);\n"
		"  float3 dir = za.xyz + xa.xyz * (ndc.x * proj.x * proj.y) + ya.xyz * (ndc.y * proj.x);\n"
		"  float3 P = eye.xyz + dir * z;\n"                                     // this pixel in the world
		"  if (abs(P.y - board.z) > board.w) discard;\n"                        // not floor height: a character, prop...
		"  float2 r = float2(P.x - board.x, P.z - board.y);\n"
		"  float u = inv.x * r.x + inv.y * r.y; float v = inv.z * r.x + inv.w * r.y;\n"
		"  if (u < 0 || u > 1 || v < 0 || v > 1) discard;\n"
		"  float4 c = boardImg.Sample(smp, float2(u, 1 - v));\n"
		"  if (c.a < 0.002) discard;\n"
		"  return float4(c.rgb * misc.x, c.a);\n"
		"}\n";
	ID3DBlob* code = nullptr;
	if (!comp(bvs, "vs_4_0", &code)) return false;
	PDev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &IFBoardVS);
	D3D11_INPUT_ELEMENT_DESC el[2] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
	PDev->CreateInputLayout(el, 2, code->GetBufferPointer(), code->GetBufferSize(), &IFLayout); code->Release();
	if (!comp(bps, "ps_4_0", &code)) return false;
	PDev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &IFBoardPS); code->Release();
	if (!comp(dvs, "vs_4_0", &code)) return false;
	PDev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &IFDecalVS); code->Release();
	if (!comp(dps, "ps_4_0", &code)) return false;
	PDev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &IFDecalPS); code->Release();

	D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 9 * 16; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	PDev->CreateBuffer(&cb, nullptr, &IFCB);
	D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = BOARD_TEX; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	PDev->CreateTexture2D(&td, nullptr, &IFBoardTex);
	if (IFBoardTex) { PDev->CreateRenderTargetView(IFBoardTex, nullptr, &IFBoardRTV); PDev->CreateShaderResourceView(IFBoardTex, nullptr, &IFBoardSRV); }
	D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
	PDev->CreateSamplerState(&sd, &IFSampler);
	D3D11_BLEND_DESC bd{}; bd.IndependentBlendEnable = FALSE;
	bd.RenderTarget[0].BlendEnable = TRUE; bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	PDev->CreateBlendState(&bd, &IFBlendBoard);
	D3D11_BLEND_DESC bd2{}; bd2.IndependentBlendEnable = TRUE;
	bd2.RenderTarget[0] = bd.RenderTarget[0];
	bd2.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd2.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	bd2.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE; // leave the game's alpha alone
	for (int i = 1; i < 8; i++) { bd2.RenderTarget[i] = bd2.RenderTarget[0]; bd2.RenderTarget[i].BlendEnable = FALSE; bd2.RenderTarget[i].RenderTargetWriteMask = 0; } // never touch other targets
	PDev->CreateBlendState(&bd2, &IFBlendDecal);
	D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
	PDev->CreateDepthStencilState(&dd, &IFNoDepth);
	D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
	PDev->CreateRasterizerState(&rd, &IFRaster);
	return IFBoardVS && IFBoardPS && IFLayout && IFDecalVS && IFDecalPS && IFCB && IFBoardTex && IFBoardRTV && IFBoardSRV && IFSampler && IFBlendBoard && IFBlendDecal && IFNoDepth && IFRaster;
}
static bool EnsureDepthCopy(ID3D11Texture2D* src)
{
	D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
	if (IFDepthCopy && IFDepthW == sd.Width && IFDepthH == sd.Height) return true;
	SafeRel(IFDepthSRV); SafeRel(IFDepthCopy);
	if (sd.Format != DXGI_FORMAT_R24G8_TYPELESS || sd.SampleDesc.Count != 1) { IFErr = "Unexpected depth format."; return false; }
	D3D11_TEXTURE2D_DESC d = sd; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0;
	if (FAILED(PDev->CreateTexture2D(&d, nullptr, &IFDepthCopy))) return false;
	D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
	if (FAILED(PDev->CreateShaderResourceView(IFDepthCopy, &vd, &IFDepthSRV))) return false;
	IFDepthW = sd.Width; IFDepthH = sd.Height;
	return true;
}
// Camera + board numbers for the decal pass, taken when the board is collected (end of the last frame).
static float IFCam[9 * 4];
static void FillDecalConstants()
{
	if (Corners.size() < 4 || !MI || !NL || NL->Height == 0) return;
	V3 z = Norm(PFront), x = Norm(Cross(V3{ 0, 1, 0 }, z)), y = Cross(z, x);
	float fov = MI->FOV > 0.1f ? MI->FOV : 0.873f;
	float A = DFar / (DFar - DNear), B = DNear * DFar / (DFar - DNear);
	V3 c1 = Corners[0], eU = Corners[3] - Corners[0], eV = Corners[1] - Corners[0];
	float det = eU.x * eV.z - eV.x * eU.z; if (std::fabs(det) < 1e-6f) det = 1e-6f;
	float floorY = 0; for (auto& c : Corners) floorY += c.y; floorY /= 4;
	float v[9 * 4] = {
		PEye.x, PEye.y, PEye.z, 0,  x.x, x.y, x.z, 0,  y.x, y.y, y.z, 0,  z.x, z.y, z.z, 0,
		std::tan(fov * 0.5f), (float)NL->Width / NL->Height, A - 1e-7f, B,
		c1.x, c1.z, floorY, IFHeightTol,
		eV.z / det, -eV.x / det, -eU.z / det, eU.x / det,
		0, 0, 0, 0,             // sizes: filled at draw time
		InFrameGain, 0, 0, 0 };
	// note: proj.z is used both as "A" and as the sky cut-off; d >= A means no surface
	v[18] = A;
	memcpy(IFCam, v, sizeof(v));
}
// Called from inside the game's frame, right after it bound its scene depth buffer.
static void DrawInFrame(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
{
	if (WReady.empty() || !PDev || !BuildInFrame()) return;
	ID3D11Texture2D* sceneTex = SceneTex(); if (!sceneTex || !EnsureDepthCopy(sceneTex)) return;
	UINT count = (UINT)WReady.size();
	if (count > IFVBCap)
	{
		SafeRel(IFVB);
		IFVBCap = count + 4096;
		D3D11_BUFFER_DESC bd{}; bd.ByteWidth = IFVBCap * sizeof(WVert); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(PDev->CreateBuffer(&bd, nullptr, &IFVB))) { IFVBCap = 0; return; }
	}
	// size of the target the game is drawing into right now
	ID3D11Resource* res = nullptr; rtv[0]->GetResource(&res);
	if (!res) return;
	ID3D11Texture2D* tex = nullptr; res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex); res->Release();
	if (!tex) return;
	D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td); tex->Release();
	InjRtvW = td.Width; InjRtvH = td.Height; InjRtvFmt = td.Format; InjRtvN = n;

	D3D11_MAPPED_SUBRESOURCE ms;
	if (FAILED(c->Map(IFVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
	memcpy(ms.pData, WReady.data(), count * sizeof(WVert));
	c->Unmap(IFVB, 0);
	IFCam[7 * 4 + 0] = (float)td.Width; IFCam[7 * 4 + 1] = (float)td.Height; IFCam[7 * 4 + 2] = (float)IFDepthW; IFCam[7 * 4 + 3] = (float)IFDepthH;
	if (FAILED(c->Map(IFCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
	memcpy(ms.pData, IFCam, sizeof(IFCam));
	c->Unmap(IFCB, 0);

	// ---- save everything we change ----
	ID3D11InputLayout* oLayout = nullptr; c->IAGetInputLayout(&oLayout);
	D3D11_PRIMITIVE_TOPOLOGY oTopo; c->IAGetPrimitiveTopology(&oTopo);
	ID3D11Buffer* oVB = nullptr; UINT oStride = 0, oOffset = 0; c->IAGetVertexBuffers(0, 1, &oVB, &oStride, &oOffset);
	ID3D11VertexShader* oVS = nullptr; c->VSGetShader(&oVS, nullptr, nullptr);
	ID3D11PixelShader* oPS = nullptr; c->PSGetShader(&oPS, nullptr, nullptr);
	ID3D11GeometryShader* oGS = nullptr; c->GSGetShader(&oGS, nullptr, nullptr);
	ID3D11HullShader* oHS = nullptr; c->HSGetShader(&oHS, nullptr, nullptr);
	ID3D11DomainShader* oDS = nullptr; c->DSGetShader(&oDS, nullptr, nullptr);
	ID3D11Buffer* oPSCB = nullptr; c->PSGetConstantBuffers(0, 1, &oPSCB);
	ID3D11ShaderResourceView* oSRV[2] = { nullptr, nullptr }; c->PSGetShaderResources(0, 2, oSRV);
	ID3D11SamplerState* oSmp = nullptr; c->PSGetSamplers(0, 1, &oSmp);
	ID3D11BlendState* oBlend = nullptr; FLOAT oFactor[4]; UINT oMask = 0; c->OMGetBlendState(&oBlend, oFactor, &oMask);
	ID3D11DepthStencilState* oDepth = nullptr; UINT oRef = 0; c->OMGetDepthStencilState(&oDepth, &oRef);
	ID3D11RasterizerState* oRaster = nullptr; c->RSGetState(&oRaster);
	UINT oVpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT oVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; c->RSGetViewports(&oVpN, oVp);

	const FLOAT f0[4] = { 0, 0, 0, 0 };
	c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
	c->OMSetDepthStencilState(IFNoDepth, 0);
	c->RSSetState(IFRaster);

	// 1. board image
	ID3D11ShaderResourceView* nullSrv[2] = { nullptr, nullptr };
	c->PSSetShaderResources(0, 2, nullSrv);
	OrigOMSetRT(c, 1, &IFBoardRTV, nullptr);
	c->ClearRenderTargetView(IFBoardRTV, f0);
	D3D11_VIEWPORT bvp{}; bvp.Width = bvp.Height = (float)BOARD_TEX; bvp.MaxDepth = 1; c->RSSetViewports(1, &bvp);
	UINT stride = sizeof(WVert), offset = 0;
	c->IASetInputLayout(IFLayout); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->IASetVertexBuffers(0, 1, &IFVB, &stride, &offset);
	c->VSSetShader(IFBoardVS, nullptr, 0); c->PSSetShader(IFBoardPS, nullptr, 0);
	c->OMSetBlendState(IFBlendBoard, f0, 0xFFFFFFFF);
	c->Draw(count, 0);

	// 2. the game's depth so far (world only)
	c->CopyResource(IFDepthCopy, sceneTex);

	// 3. back to the game's targets, paint the board onto the floor
	OrigOMSetRT(c, n, rtv, dsv);
	D3D11_VIEWPORT vp{}; vp.Width = (float)td.Width; vp.Height = (float)td.Height; vp.MaxDepth = 1; c->RSSetViewports(1, &vp);
	c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	c->VSSetShader(IFDecalVS, nullptr, 0); c->PSSetShader(IFDecalPS, nullptr, 0);
	c->PSSetConstantBuffers(0, 1, &IFCB);
	ID3D11ShaderResourceView* srvs[2] = { IFDepthSRV, IFBoardSRV }; c->PSSetShaderResources(0, 2, srvs);
	c->PSSetSamplers(0, 1, &IFSampler);
	c->OMSetBlendState(IFBlendDecal, f0, 0xFFFFFFFF);
	c->Draw(3, 0);

	// ---- put everything back ----
	c->PSSetShaderResources(0, 2, oSRV); c->PSSetSamplers(0, 1, &oSmp); c->PSSetConstantBuffers(0, 1, &oPSCB);
	c->IASetInputLayout(oLayout); c->IASetPrimitiveTopology(oTopo); c->IASetVertexBuffers(0, 1, &oVB, &oStride, &oOffset);
	c->VSSetShader(oVS, nullptr, 0); c->PSSetShader(oPS, nullptr, 0); c->GSSetShader(oGS, nullptr, 0); c->HSSetShader(oHS, nullptr, 0); c->DSSetShader(oDS, nullptr, 0);
	c->OMSetBlendState(oBlend, oFactor, oMask); c->OMSetDepthStencilState(oDepth, oRef); c->RSSetState(oRaster);
	if (oVpN) c->RSSetViewports(oVpN, oVp);
	SafeRel(oLayout); SafeRel(oVB); SafeRel(oVS); SafeRel(oPS); SafeRel(oGS); SafeRel(oHS); SafeRel(oDS); SafeRel(oPSCB);
	SafeRel(oSRV[0]); SafeRel(oSRV[1]); SafeRel(oSmp); SafeRel(oBlend); SafeRel(oDepth); SafeRel(oRaster);
}
// Whether this frame's board goes into the game's frame. Falls back to the overlay if drawing inside
// the frame stopped working (e.g. the chosen bind never happens).
static bool InFrameUsable()
{
	return InFrame && UnderChars && ProbeOn && PCtx && SceneDsv() && BuildInFrame() && InjectMiss < 60;
}

static void DepthUI()
{
	ImGui::Separator();
	ImGui::TextDisabled("Board under characters (experimental)");
	if (ImGui::Checkbox("Draw the board under characters", &UnderChars)) SaveSettings();
	if (!UnderChars) return;
	ImGui::SliderInt("Scene buffer", &DepthBuf, 0, CandN > 0 ? CandN - 1 : 0, "Buffer %d (0 = A)");
	ImGui::Checkbox("Auto near plane (recommended)", &AutoNear);
	if (AutoNearUsed > 0) { ImGui::SameLine(); ImGui::TextDisabled("measured %.3f from %d floor points", AutoNearValue, AutoNearUsed); }
	// number boxes: type a value, or use - / + (hold Ctrl for 10x steps)
	ImGui::SetNextItemWidth(160); ImGui::InputFloat("Near plane (m)", &DNear, 0.001f, 0.01f, "%.3f");
	ImGui::SetNextItemWidth(160); ImGui::InputFloat("Far plane (m)", &DFar, 10.0f, 100.0f, "%.0f");
	ImGui::SetNextItemWidth(160); ImGui::InputFloat("Tolerance (share of distance)", &DTol, 0.001f, 0.01f, "%.3f");
	ImGui::SetNextItemWidth(160); ImGui::InputFloat("Tolerance (meters)", &DBias, 0.01f, 0.1f, "%.2f");
	if (DNear < 0.01f) DNear = 0.01f; if (DFar < DNear + 1) DFar = DNear + 1; if (DTol < 0) DTol = 0; if (DBias < 0) DBias = 0;
	ImGui::TextDisabled("Type a value or use - / +. Untick Auto near plane to set the near plane yourself.");
	ImGui::Text(DepthActive() ? "Active (overlay)." : (InFrameUsable() ? "Active (inside the game's frame)." : "Not active yet."));
	ImGui::Separator();
	ImGui::Checkbox("Draw inside the game's frame (experimental)", &InFrame);
	if (InFrame)
	{
		ImGui::SetNextItemWidth(160); ImGui::InputInt("Draw at scene depth bind #", &InjectAt);
		if (InjectAt < 1) InjectAt = 1; if (InjectAt > 8) InjectAt = 8;
		ImGui::SetNextItemWidth(160); ImGui::InputFloat("Color strength inside the frame", &InFrameGain, 0.05f, 0.25f, "%.2f");
		ImGui::SetNextItemWidth(160); ImGui::InputFloat("Floor height tolerance (m)", &IFHeightTol, 0.01f, 0.1f, "%.2f");
		if (ImGui::Button("Retry shaders")) IFErr.clear();
		ImGui::Text("Scene depth bound %d times last frame. Drawn: %s. Target %u x %u, format %u, %u target(s).",
			LastSceneBinds, InjectMiss == 0 ? "yes" : "no", InjRtvW, InjRtvH, InjRtvFmt, InjRtvN);
		if (!IFErr.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", IFErr.c_str());
		ImGui::TextDisabled("Bind 2 should be: after the world, before the game's ground marks. Try 1-3: pick the one where characters, telegraphs, name plates and the commander tag show on top.");
	}
	if (!DepthErr.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", DepthErr.c_str());
}

/* ---------------------------------------------------------------------------------------------- */
/* Player panel (normal mode)                                                                     */
/* ---------------------------------------------------------------------------------------------- */

static void DrawPanel()
{
	if (!ShowPanel) return;
	ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Raid Workshop", &ShowPanel, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
	if (!ImGui::CollapsingHeader("Mursaat Overseer - Chess", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Other fights: coming later.");
		if (ImGui::Checkbox("Debug mode (ALT+SHIFT+D)", &DebugMode)) SaveSettings();
		ImGui::End(); return;
	}
	bool place = BoardMoved || (MI && MI->MapID == MO_MAP);
	if (!place) ImGui::TextWrapped("Go to the Mursaat Overseer room.");
	else if (!BoardActive) ImGui::TextWrapped("Board off. Stand in the gold circle for 5 seconds to turn it on (it disappears once the board is on).");
	else if (FightOn)
	{
		int sec = (int)(Now() - FightStart);
		ImGui::Text("%s %d:%02d  -  you are the %s", TestFight ? "Test fight" : "Fight", sec / 60, sec % 60, PIECE_NAMES[MyPiece]);
	}
	else if (!Armed) ImGui::TextWrapped("Go to one of the gold start squares.");
	else ImGui::TextWrapped("Ready. The fight starts when the boss is pulled.");

	if (FightOn) { if (ImGui::Button("Stop fight")) EndFight("Fight stopped."); }
	else if (BoardActive)
	{
		if (ImGui::Button("Start test fight")) BeginFight(true);
		ImGui::SameLine(); if (ImGui::Button("Turn board off")) { BoardActive = false; Status = "Board off."; }
	}
	ImGui::Separator();
	bool ch = false;
	ch |= ImGui::Checkbox("I hold Claim (Pawn)", &IAmClaim);
	if (RelayState == 2) ImGui::Text("Squad: %d other player(s) with the addon.", PeersOnline() );
	else if (BoardActive && SquadLinkOn) ImGui::TextDisabled(RelayState == 1 ? "Squad link: connecting..." : "Squad link: offline (retrying)");
	ch |= ImGui::Checkbox("Type /gg when I fail", &RealGG);
	ch |= ImGui::Checkbox("Show my piece at the bottom of the screen", &PieceOnScreen);
	ch |= ImGui::SliderFloat("Board strength", &ChessAlpha, 0.1f, 0.8f, "%.2f");
	ch |= ImGui::Checkbox("Board under characters (experimental)", &UnderChars);
	if (UnderChars) ch |= ImGui::Checkbox("Draw inside the game's frame (experimental)", &InFrame);
	if (ch) SaveSettings();
	ImGui::Separator();
	ImGui::TextDisabled("Other fights: coming later.");
	if (ImGui::Checkbox("Debug mode (ALT+SHIFT+D)", &DebugMode)) SaveSettings();
	ImGui::End();
}

/* ---------------------------------------------------------------------------------------------- */
/* Relay: share your own position with the other players' addons (and receive theirs)            */
/* Each addon only sends what MumbleLink already gives it: name, position, and its move result.  */
/* Messages are short text lines, broadcast by a tiny server to everyone in the same room:        */
/*   P|name|x|y|z          position, 10 times a second                                           */
/*   R|name|file|rank|0/1  result of a move check                                                 */
/* ---------------------------------------------------------------------------------------------- */

static bool RelayOn = false;
static std::mutex RelayMx;               // guards the outgoing data below
static std::string MyName; static V3 MyPos{ 0, 0, 0 }; static bool MyPosValid = false;
static std::deque<std::string> RelayOut;
static std::string RelayErr;

// One connection = one object owned by its own thread. Stopping never waits: it flags the
// connection and closes its handles, and the thread cleans up on its own.
struct RelayConn
{
	std::atomic<bool> run{ true };
	std::mutex hmx; HINTERNET ses = nullptr, con = nullptr, ws = nullptr;
	std::string url, room;
	void CloseAll()
	{
		std::lock_guard<std::mutex> lk(hmx);
		if (ws) { WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0); WinHttpCloseHandle(ws); ws = nullptr; }
		if (con) { WinHttpCloseHandle(con); con = nullptr; }
		if (ses) { WinHttpCloseHandle(ses); ses = nullptr; }
	}
};
static std::shared_ptr<RelayConn> Conn;  // current connection
static std::mutex ConnMx;
static std::atomic<int> RelayThreads{ 0 }; // live connection threads (the addon waits for 0 on unload)
static bool IsCurrent(const std::shared_ptr<RelayConn>& c) { std::lock_guard<std::mutex> lk(ConnMx); return Conn == c; }

static std::wstring Widen(const std::string& a) { return std::wstring(a.begin(), a.end()); }
static std::string RoomKey()
{
	// Everyone in the same map instance lands in the same room automatically. Raid instances belong
	// to one squad, so in MO the room is exactly your squad. An optional code (debug) splits further.
	char buf[200]; unsigned h = 2166136261u;
	auto mix = [&](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } };
	if (ML) { mix(ML->Context.ServerAddress, 28); mix(&ML->Context.ShardID, 4); mix(&ML->Context.InstanceID, 4); }
	std::string room = "I";
	for (const char* p = RelayRoom; *p; p++) if (isalnum((unsigned char)*p)) room += (char)toupper((unsigned char)*p);
	snprintf(buf, sizeof(buf), "%s-%u-%08x", room.c_str(), MI ? MI->MapID : 0, h);
	return buf;
}
static void SetErr(const std::string& e) { std::lock_guard<std::mutex> lk(RelayMx); RelayErr = e; }
static bool ConnOpen(RelayConn& c)
{
	bool secure = c.url.rfind("wss://", 0) == 0;
	if (!secure && c.url.rfind("ws://", 0) != 0) { SetErr("The address must start with wss:// or ws://"); return false; }
	std::string rest = c.url.substr(secure ? 6 : 5);
	std::string host = rest.substr(0, rest.find('/'));
	INTERNET_PORT port = secure ? 443 : 80;
	size_t colon = host.find(':'); if (colon != std::string::npos) { port = (INTERNET_PORT)atoi(host.c_str() + colon + 1); host = host.substr(0, colon); }
	std::wstring path = Widen("/room/" + c.room);
	HINTERNET ses = WinHttpOpen(L"RaidWorkshop/0.9", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) ses = WinHttpOpen(L"RaidWorkshop/0.9", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!ses) { SetErr("Could not start networking."); return false; }
	WinHttpSetTimeouts(ses, 5000, 5000, 5000, 0);   // receive has no timeout: it waits for messages
	{ std::lock_guard<std::mutex> lk(c.hmx); c.ses = ses; }
	HINTERNET con = WinHttpConnect(ses, Widen(host).c_str(), port, 0);
	if (!con) { SetErr("Could not reach " + host); return false; }
	{ std::lock_guard<std::mutex> lk(c.hmx); c.con = con; }
	HINTERNET req = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
	if (!req) { SetErr("Request failed."); return false; }
	WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr))
	{ SetErr("Server did not answer."); WinHttpCloseHandle(req); return false; }
	HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
	WinHttpCloseHandle(req);
	if (!ws) { SetErr("Server refused the connection."); return false; }
	std::lock_guard<std::mutex> lk(c.hmx);
	if (!c.run) { WinHttpCloseHandle(ws); return false; }
	c.ws = ws;
	return true;
}
static void HandleLine(const std::string& line)
{
	std::vector<std::string> p; size_t a = 0;
	while (true) { size_t b = line.find('|', a); p.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
	std::string me; { std::lock_guard<std::mutex> lk(RelayMx); me = MyName; }
	if (p.size() < 2 || p[1].empty() || p[1] == me) return;
	std::lock_guard<std::mutex> lk(PeersMx);
	Peer& pe = Peers[p[1]]; pe.name = p[1];
	if (p[0] == "P" && p.size() >= 5) { pe.pos = { (float)atof(p[2].c_str()), (float)atof(p[3].c_str()), (float)atof(p[4].c_str()) }; pe.seen = NowSec(); }
	else if (p[0] == "R" && p.size() >= 5) { pe.resF = atoi(p[2].c_str()); pe.resR = atoi(p[3].c_str()); pe.resPass = p[4] == "1"; pe.resT = NowSec(); pe.seen = NowSec(); }
}
static void ConnThread(std::shared_ptr<RelayConn> c)
{
	struct Count { Count() { RelayThreads++; } ~Count() { RelayThreads--; } } count;
	if (!ConnOpen(*c)) { if (c->run && IsCurrent(c)) RelayState = 3; c->CloseAll(); return; }
	if (!IsCurrent(c)) { c->CloseAll(); return; }
	RelayState = 2; SetErr("");
	std::thread tx([c] {
		while (c->run)
		{
			std::string out;
			{
				std::lock_guard<std::mutex> lk(RelayMx);
				if (MyPosValid && !MyName.empty()) { char b[160]; snprintf(b, sizeof(b), "P|%s|%.2f|%.2f|%.2f", MyName.c_str(), MyPos.x, MyPos.y, MyPos.z); out = b; }
				while (!RelayOut.empty()) { if (!out.empty()) out += "\n"; out += RelayOut.front(); RelayOut.pop_front(); }
			}
			HINTERNET ws; { std::lock_guard<std::mutex> lk(c->hmx); ws = c->ws; }
			if (!ws) break;
			if (!out.empty() && WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)out.data(), (DWORD)out.size()) != NO_ERROR) break;
			Sleep(100);
		}
	});
	char buf[1024]; std::string msg;
	while (c->run)
	{
		HINTERNET ws; { std::lock_guard<std::mutex> lk(c->hmx); ws = c->ws; }
		if (!ws) break;
		DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
		if (WinHttpWebSocketReceive(ws, buf, sizeof(buf), &got, &type) != NO_ERROR) break;
		if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
		msg.append(buf, got);
		if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
		{
			size_t a = 0, b;
			while ((b = msg.find('\n', a)) != std::string::npos) { HandleLine(msg.substr(a, b - a)); a = b + 1; }
			if (a < msg.size()) HandleLine(msg.substr(a));
			msg.clear();
		}
	}
	bool wasRunning = c->run.exchange(false);
	c->CloseAll();
	tx.join();
	if (wasRunning && IsCurrent(c)) { RelayState = 3; SetErr("Disconnected."); }
}
static void RelayStop()
{
	std::shared_ptr<RelayConn> c;
	{ std::lock_guard<std::mutex> lk(ConnMx); c.swap(Conn); }
	if (c) { c->run = false; c->CloseAll(); }   // the thread notices and exits by itself
	RelayState = 0;
	std::lock_guard<std::mutex> lk(PeersMx);
	for (auto it = Peers.begin(); it != Peers.end();) { if (!it->second.dummy) it = Peers.erase(it); else ++it; }
}
static void RelayStart()
{
	RelayStop();
	auto c = std::make_shared<RelayConn>();
	c->url = RelayUrl[0] ? RelayUrl : DEFAULT_RELAY; c->room = RoomKey();
	{ std::lock_guard<std::mutex> lk(ConnMx); Conn = c; }
	RelayState = 1;
	std::thread(ConnThread, c).detach();
}
static void RelaySendResult(int f, int r, bool pass)
{
	if (RelayState != 2) return;
	std::lock_guard<std::mutex> lk(RelayMx);
	char b[120]; snprintf(b, sizeof(b), "R|%s|%d|%d|%d", MyName.c_str(), f, r, pass ? 1 : 0);
	RelayOut.push_back(b);
}
static std::string RelayRoomNow; static double RelayRetryAt = 0;
static void RelayAuto()
{
	// connect by itself while the board is on (MO room or test board); leave when it is off,
	// move to the new room when the instance changes, retry after errors.
	bool want = SquadLinkOn && BoardActive;
	double t = NowSec();
	if (!want) { if (RelayState != 0) RelayStop(); RelayRoomNow.clear(); return; }
	std::string room = RoomKey();
	if (RelayState == 2 && room != RelayRoomNow) { RelayStop(); }
	if ((RelayState == 0 || RelayState == 3) && t >= RelayRetryAt)
	{
		RelayRetryAt = t + 10.0;
		RelayRoomNow = room;
		RelayStart();
	}
}
static void RelayFrame() // once per frame: hand our position to the sender, drop silent peers
{
	RelayAuto();
	{
		std::lock_guard<std::mutex> lk(RelayMx);
		if (MI) { char n[21]; memcpy(n, MI->Name, 20); n[20] = 0; MyName = n; for (char& c : MyName) if (c == '|' || c == '\n') c = ' '; }
		if (ML) { MyPos = FromMumble(ML->AvatarPosition); MyPosValid = true; }
	}
	std::lock_guard<std::mutex> lk(PeersMx);
	double t = NowSec();
	for (auto it = Peers.begin(); it != Peers.end();) { if (!it->second.dummy && t - it->second.seen > 6.0) it = Peers.erase(it); else ++it; }
}
static int PeersOnline()
{
	std::lock_guard<std::mutex> lk(PeersMx); int n = 0; double t = NowSec();
	for (auto& kv : Peers) if (!kv.second.dummy && t - kv.second.seen < 3.0) n++;
	return n;
}
static void AddDummy(V3 p)
{
	std::lock_guard<std::mutex> lk(PeersMx);
	int i = 1; while (Peers.count("dummy " + std::to_string(i))) i++;
	Peer& d = Peers["dummy " + std::to_string(i)]; d.name = "dummy " + std::to_string(i); d.pos = p; d.dummy = true; d.seen = NowSec();
}
static void ClearDummies()
{
	std::lock_guard<std::mutex> lk(PeersMx);
	for (auto it = Peers.begin(); it != Peers.end();) { if (it->second.dummy) it = Peers.erase(it); else ++it; }
}
// Other players (and dummies): a small marker at their feet with their name, drawn over the world.
static void DrawPeers(ImDrawList* fg)
{
	std::lock_guard<std::mutex> lk(PeersMx);
	double t = NowSec();
	for (auto& kv : Peers)
	{
		const Peer& p = kv.second;
		if (!p.dummy && t - p.seen > 3.0) continue;
		ImU32 col = p.dummy ? IM_COL32(190, 120, 255, 230) : IM_COL32(120, 200, 255, 230);
		WorldRing(fg, p.pos, 0.45f, IM_COL32(0, 0, 0, 160), 5.0f);
		WorldRing(fg, p.pos, 0.45f, col, 2.5f);
		ImVec2 s;
		if (WorldToScreen(p.pos + V3{ 0, 2.2f, 0 }, s))
		{
			ImVec2 ts = ImGui::CalcTextSize(p.name.c_str());
			fg->AddText(ImVec2(s.x - ts.x / 2 + 1, s.y + 1), IM_COL32(0, 0, 0, 200), p.name.c_str());
			fg->AddText(ImVec2(s.x - ts.x / 2, s.y), col, p.name.c_str());
		}
	}
}
static void RelayUI()
{
	ImGui::Separator();
	ImGui::TextDisabled("Squad link (automatic: everyone in this instance with the addon)");
	if (ImGui::Checkbox("Share my position with my squad", &SquadLinkOn)) SaveSettings();
	if (ImGui::TreeNode("Advanced"))
	{
		ImGui::SetNextItemWidth(260); ImGui::InputText("Relay address (empty = default)", RelayUrl, sizeof(RelayUrl));
		ImGui::SetNextItemWidth(120); ImGui::InputText("Extra code (optional)", RelayRoom, sizeof(RelayRoom));
		if (ImGui::Button("Reconnect")) { RelayStop(); RelayRetryAt = 0; SaveSettings(); }
		ImGui::TreePop();
	}
	const char* st[] = { "Not connected.", "Connecting...", "Connected.", "Error: " };
	int sidx = RelayState; if (sidx < 0 || sidx > 3) sidx = 0;
	std::string err; { std::lock_guard<std::mutex> lk(RelayMx); err = RelayErr; }
	if (sidx == 3) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s%s", st[3], err.c_str());
	else ImGui::Text("%s  %d other player(s) online.", st[sidx], PeersOnline());
	if (ImGui::Button("Add test player on my square")) AddDummy(FromMumble(ML->AvatarPosition));
	ImGui::SameLine(); if (ImGui::Button("Remove test players")) ClearDummies();
	ImGui::TextDisabled("Test players stand still. End your move on their square to see both fail.");
}

static UINT WndProcHook(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	GameHwnd = hWnd;
	return uMsg; // pass everything on
}

/* ---------------------------------------------------------------------------------------------- */
/* Window                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

static void DrawWindow()
{
	ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
	bool open = true;
	bool vis = ImGui::Begin("Raid Workshop - MO debug", &open);
	if (!open) { DebugMode = false; SaveSettings(); } // closing the debug window = back to the normal look
	if (!vis) { ImGui::End(); return; }

	V3 p = SmoothedPosition();
	ImGui::Text("Map %u   Position %.2f, %.2f, %.2f m", MI ? MI->MapID : 0, p.x, p.y, p.z);
	ImGui::Separator();

	if (Corners.size() < 4)
	{
		ImGui::TextWrapped("Stand exactly on this corner of the MO board and press Mark corner (ALT+SHIFT+M):");
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.7f, 1), "%s", CORNER_NAMES[Corners.size()]);
		ImGui::TextWrapped("Start side = where the squad starts and the Soldiers form. Far side = where the Scouts spawn.");
	}
	for (size_t i = 0; i < Corners.size(); i++)
	{
		ImGui::Text("Corner %zu: %.2f, %.2f, %.2f", i + 1, Corners[i].x, Corners[i].y, Corners[i].z);
	}

	if (ImGui::Button("Mark corner")) { MarkCorner(); }
	ImGui::SameLine(); if (ImGui::Button("Undo")) { ProcessKeybind(KB_UNDO, false); }
	ImGui::SameLine(); if (ImGui::Button("Clear")) { Corners.clear(); Beams.clear(); HasInner = false; ComputeInset(); Status = "Cleared."; }
	ImGui::SameLine(); if (ImGui::Button("Save")) { Save(); }
	ImGui::SameLine(); if (ImGui::Button("Load")) { Load(); }
	ImGui::Checkbox("Mark with the mouse", &PickMode);
	if (PickMode)
	{
		ImGui::TextWrapped("Point the mouse at the spot on the floor (cyan ring) and left-click it, or press ALT+SHIFT+M. Uses the board's floor height. Untick when done.");
		if (PickValid) { ImGui::Text("Mouse on floor: %.2f, %.2f, %.2f", PickPoint.x, PickPoint.y, PickPoint.z); }
	}
	if (Corners.size() == 3 && ImGui::Button("Compute corner 4 from the other three"))
	{
		Corners.push_back(Corners[0] + Corners[2] - Corners[1]); Save();
		Status = "Corner 4 computed (the board is square). Check the grid lines on the floor.";
	}

	if (Corners.size() == 4)
	{
		ImGui::Separator();
		float sides[4] = {
			Len(Corners[1] - Corners[0]), Len(Corners[2] - Corners[1]),
			Len(Corners[3] - Corners[2]), Len(Corners[0] - Corners[3]) };
		float avg = (sides[0] + sides[1] + sides[2] + sides[3]) / 4.0f;
		ImGui::Text("Board sides: %.1f, %.1f, %.1f, %.1f m (average %.1f m)", sides[0], sides[1], sides[2], sides[3], avg);
		float tile = avg / 4, frame = tile * (InsetU + InsetV) / 2;
		ImGui::Text("Big tile: %.2f m    frame: %.2f m    plate (chess square): %.2f m", tile, frame, (tile - 2 * frame) / 3);
		float diag1 = Len(Corners[2] - Corners[0]), diag2 = Len(Corners[3] - Corners[1]);
		ImGui::Text("Diagonals: %.1f / %.1f m (equal means the corners are square)", diag1, diag2);

		if (!HasInner)
		{
			ImGui::TextWrapped("Next: stand on the inner corner of the plates closest to corner 1 (where the frame band ends and the first plate starts) and press Mark.");
		}
		else { ImGui::Text("Frame: %.0f%% of a tile across, %.0f%% along", InsetU * 100, InsetV * 100); }
		ImGui::Checkbox("Draw grid on the floor", &ShowGrid); ImGui::SameLine();
		ImGui::Checkbox("Highlight my square", &ShowSquare);

		float u, v;
		if (BoardUV(p, u, v) && u >= 0 && u < 1 && v >= 0 && v < 1)
		{
			int f = PlateIndex(u, InsetU), r = PlateIndex(v, InsetV);
			int tf = (int)(u * 4), tr = (int)(v * 4);
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.7f, 1), "You are on %s  (MO tile %d,%d)", SquareName(f, r).c_str(), tf + 1, tr + 1);
		}
		else { ImGui::Text("You are off the board."); }

		ImGui::Separator();
		if (HasInner && Beams.size() < 3)
		{
			ImGui::TextWrapped("Now stand in the light beam where this role is picked up and press Mark (ALT+SHIFT+M):");
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s beam", BEAM_NAMES[Beams.size()]);
		}
		for (size_t i = 0; i < Beams.size(); i++)
		{
			float bu, bv;
			std::string where = "off the board";
			if (BoardUV(Beams[i], bu, bv) && bu >= 0 && bu < 1 && bv >= 0 && bv < 1)
			{
				where = SquareName(PlateIndex(bu, InsetU), PlateIndex(bv, InsetV));
			}
			ImGui::Text("%s beam: %s", BEAM_NAMES[i], where.c_str());
		}
	}

	DrawPracticeUI();
	if (Corners.size() == 4)
	{
		ImGui::Separator();
		ImGui::TextDisabled("Board switch");
		ImGui::Text("Board is %s. Pad: %s", BoardActive ? "ON" : "off", HasPad ? "custom" : "default (square a2)");
		if (ImGui::Button(BoardActive ? "Turn board off" : "Turn board on now")) { BoardActive = !BoardActive; if (BoardActive) { ActivatedAt = NowSec(); Armed = false; } }
		ImGui::SameLine(); if (ImGui::Button("Move the gold circle here")) { Pad = FromMumble(ML->AvatarPosition); HasPad = true; Save(); }
		if (HasPad) { ImGui::SameLine(); if (ImGui::Button("Reset circle")) { HasPad = false; Save(); } }
		ImGui::TextDisabled("Close this window (or ALT+SHIFT+D) for the normal look.");
	}
	ProbeUI();
	DepthUI();
	RelayUI();
	if (!Status.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s", Status.c_str()); }
	ImGui::End();
}

static void Render()
{
	if (!ML || !NL) { return; }
	UpdateMotion();
	RecentPos.push_back(FromMumble(ML->AvatarPosition));
	while (RecentPos.size() > 10) { RecentPos.pop_front(); }

	static double lastFrame = NowSec();
	double nowT = NowSec(); float dt = (float)(nowT - lastFrame); lastFrame = nowT; if (dt > 0.2f) dt = 0.2f;
	ProbeFrame();
	DepthFrame();
	RelayFrame();
	UpdateActivation(dt);
	UpdatePlay();
	if (ML->Context.IsMapOpen || !NL->IsGameplay) { WReady.clear(); return; }
	bool place = BoardMoved || (MI && MI->MapID == MO_MAP);
	WBuild.clear();
	Capturing = InFrameUsable();
	if (Capturing) FillDecalConstants();
	struct CapEnd { ~CapEnd() { Capturing = false; WReady.swap(WBuild); } } capEnd; // hand this frame's board to the next frame
	if (DebugMode)
	{
		if (place)
		{
			DrawBoard();          // grid, plates, labels, beams, your square
			if (!BoardActive) DrawPad();
			DrawPlay();
			DrawPeers(ImGui::GetForegroundDrawList());
			if (ShowCalWindow) { DrawMarkers(); }
		}
		DrawWindow();
		DrawPanel();
		if (place) { UpdatePick(); }
		return;
	}
	if (!place) { DrawPanel(); return; }
	if (BoardActive) { DrawBoardPolish(); DrawPlay(); DrawPeers(ImGui::GetForegroundDrawList()); }
	else { DrawPad(); }   // the circle only exists until the board is on
	DrawPanel();
}

static void Options()
{
	if (ImGui::Checkbox("Debug mode", &DebugMode)) SaveSettings();
	ImGui::Checkbox("Show the Raid Workshop panel (ALT+SHIFT+B)", &ShowPanel);
}

/* ---------------------------------------------------------------------------------------------- */
/* Load / Unload                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

static void AddonLoad(AddonAPI_t* aApi)
{
	API = aApi;
	ImGui::SetCurrentContext((ImGuiContext*)API->ImguiContext);
	ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))API->ImguiMalloc, (void (*)(void*, void*))API->ImguiFree);

	ML = (Mumble::Data*)API->DataLink_Get(DL_MUMBLE_LINK);
	MI = (Mumble::Identity*)API->DataLink_Get(DL_MUMBLE_LINK_IDENTITY);
	NL = (NexusLinkData_t*)API->DataLink_Get(DL_NEXUS_LINK);

	API->InputBinds_RegisterWithString(KB_MARK, ProcessKeybind, "ALT+SHIFT+M");
	API->InputBinds_RegisterWithString(KB_UNDO, ProcessKeybind, "ALT+SHIFT+N");
	API->InputBinds_RegisterWithString(KB_WINDOW, ProcessKeybind, "ALT+SHIFT+B");
	API->InputBinds_RegisterWithString(KB_MOVE, ProcessKeybind, "ALT+SHIFT+J");
	API->InputBinds_RegisterWithString(KB_DEBUG, ProcessKeybind, "ALT+SHIFT+D");
	API->WndProc_Register(WndProcHook);

	API->GUI_Register(RT_Render, Render);
	API->GUI_Register(RT_OptionsRender, Options);

	Load();
	LoadSettings();
	LastMap = MI ? MI->MapID : 0;
	API->Log(LOGL_INFO, LOGCH, "Raid Workshop loaded.");
}

static void AddonUnload()
{
	API->GUI_Deregister(Render);
	API->GUI_Deregister(Options);
	API->InputBinds_Deregister(KB_MARK);
	API->InputBinds_Deregister(KB_UNDO);
	API->InputBinds_Deregister(KB_WINDOW);
	API->InputBinds_Deregister(KB_MOVE);
	API->InputBinds_Deregister(KB_DEBUG);
	API->WndProc_Deregister(WndProcHook);
	ProbeStop();
	RelayStop();
	for (int i = 0; i < 300 && RelayThreads > 0; i++) Sleep(10); // let connection threads finish before the DLL goes away
	Outbox.clear();
	ML = nullptr; MI = nullptr; NL = nullptr;
}

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
	AddonDef.Signature = (uint32_t)-0x52574B31; // unique negative id: not hosted on Raidcore
	AddonDef.APIVersion = NEXUS_API_VERSION;
	AddonDef.Name = "Raid Workshop";
	AddonDef.Version = { 0, 4, 0, 0 };
	AddonDef.Author = "KP Sales Squad";
	AddonDef.Description = "Player-made challenges on top of raid fights. Now: Mursaat Overseer Chess.";
	AddonDef.Load = AddonLoad;
	AddonDef.Unload = AddonUnload;
	AddonDef.Flags = AF_None;
	AddonDef.Provider = UP_None;
	AddonDef.UpdateLink = nullptr;
	return &AddonDef;
}
