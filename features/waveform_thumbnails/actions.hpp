#pragma once

// Include after reaper_plugin.h.
namespace mwt::actions {
constexpr int mediaExplorerSection = 32063;

struct Action {
  const char* mainId;
  gaccel_register_t* main;
  custom_action_register_t explorer;
  int explorerId = 0;
};

inline Action entries[4]{};
inline bool (*runMain)(int, int) = nullptr;
inline void (*refreshToolbar)(int, int) = nullptr;

inline bool onExplorerCommand(KbdSectionInfo* section, int cmd, int, int, int, HWND) {
  if (!section || section->uniqueID != mediaExplorerSection || cmd <= 0) return false;
  for (const auto& action : entries) {
    if (cmd == action.explorerId) return runMain(action.main->accel.cmd, 0);
  }
  return false;
}

inline void registerActions(const reaper_plugin_info_t& host, bool (*handler)(int, int),
                            gaccel_register_t& toggle, gaccel_register_t& refresh,
                            gaccel_register_t& colors, gaccel_register_t& reset) {
  entries[0] = {"MWT_TOGGLE", &toggle, {mediaExplorerSection, "MWT_ME_TOGGLE", "ME+: Toggle waveform thumbnails", nullptr}};
  entries[1] = {"MWT_REFRESH", &refresh, {mediaExplorerSection, "MWT_ME_REFRESH", "ME+: Refresh waveform thumbnail cache", nullptr}};
  entries[2] = {"MWT_COLORS", &colors, {mediaExplorerSection, "MWT_ME_COLORS", "ME+: Customize waveform colors...", nullptr}};
  entries[3] = {"MWT_COLORS_RESET", &reset, {mediaExplorerSection, "MWT_ME_COLORS_RESET", "ME+: Reset waveform colors to defaults", nullptr}};

  runMain = handler;
  refreshToolbar = reinterpret_cast<decltype(refreshToolbar)>(host.GetFunc("RefreshToolbar2"));
  host.Register("hookcommand2", reinterpret_cast<void*>(onExplorerCommand));
  for (auto& action : entries) {
    action.main->accel.cmd = WORD(host.Register("command_id", const_cast<char*>(action.mainId)));
    action.main->desc = action.explorer.name;
    host.Register("gaccel", action.main);
    if (action.main->accel.cmd)
      action.explorerId = host.Register("custom_action", &action.explorer);
  }
}

inline bool isToggle(int cmd) {
  return cmd > 0 && entries[0].main &&
         (cmd == entries[0].main->accel.cmd || cmd == entries[0].explorerId);
}

inline void refreshToggle() {
  if (!refreshToolbar) return;
  if (entries[0].main->accel.cmd) refreshToolbar(0, entries[0].main->accel.cmd);
  if (entries[0].explorerId > 0) refreshToolbar(mediaExplorerSection, entries[0].explorerId);
}

inline void unregisterActions(const reaper_plugin_info_t& host) {
  if (!runMain) return;
  host.Register("-hookcommand2", reinterpret_cast<void*>(onExplorerCommand));
  for (auto& action : entries) {
    if (action.explorerId > 0) host.Register("-custom_action", &action.explorer);
    host.Register("-gaccel", action.main);
    action = {};
  }
  runMain = nullptr;
  refreshToolbar = nullptr;
}
}
