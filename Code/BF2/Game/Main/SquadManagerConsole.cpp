// SquadManager console query bindings. [CHOSEN, not recovered]
// Retail BF2.exe carries the SquadManager console object with listSquads and listSquadMembers rows.
// Mods parse their output through host.rcon_invoke (Project Reality's getSquadName reads the
// "id:N <name> is public." lines). The binding table itself is not recovered yet: these two rows
// forward to IGame::listSquads / IGame::listSquadMembers. The other retail SquadManager rows stay
// unregistered until they are recovered.
#include "../../Core/ConsoleObjectMacro.h"
#include "../IGame.h"
#include <string>

namespace dice {
namespace hfe {

extern IGame* game;   // ds:dice::hfe::game

namespace {

const char* const kSquadManagerConsoleTag = "game.squadManager";

std::string consoleListSquads(int team)                    { return game->listSquads(team); }
std::string consoleListSquadMembers(int team, int squad)   { return game->listSquadMembers(team, squad); }

} // namespace

BF2_CONSOLE_OBJECT("SquadManager", 1, 0)
BF2_CONSOLE_METHOD_1_RET_ACTIVE(listSquads, consoleListSquads, std::string, game != 0, int,
                                kSquadManagerConsoleTag)
BF2_CONSOLE_METHOD_2_RET_ACTIVE(listSquadMembers, consoleListSquadMembers, std::string, game != 0, int, int,
                                kSquadManagerConsoleTag)

// Handed to the console registry by ModuleGame::init (Core/ModuleRegistry.cpp), next to the sv list.
ConsoleObjectList& squadManagerConsoleObjects()
{
    return consoleBindingList(kSquadManagerConsoleTag);
}

} // namespace hfe
} // namespace dice
