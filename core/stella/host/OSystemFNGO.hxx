/*
 * OSystemFNGO -- Stella's OSystem for the FujiNet Go host.
 *
 * Modelled on src/os/libretro/OSystemLIBRETRO.hxx. Settings persist in
 * Stella's own JSON key/value file under the session's data directory, so
 * the few Stella-side preferences the app exposes (palette, TV effects)
 * survive a restart, while everything the user sees in Preferences lives in
 * the family's shared settings.ini and is pushed into Stella at start.
 *
 * stateChanged() is the one hook Stella offers for "the emulator entered or
 * left the DEBUGGER/PAUSE state": EventHandler::setState() calls it, which is
 * how StellaHost learns about a breakpoint without polling.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OSYSTEM_FNGO_HXX
#define OSYSTEM_FNGO_HXX

#include <functional>

#include "FSNode.hxx"
#include "OSystem.hxx"
#include "repository/KeyValueRepositoryJsonFile.hxx"
#include "repository/CompositeKeyValueRepositoryNoop.hxx"

#include "FNGOHostHooks.hxx"

class OSystemFNGO : public OSystem
{
  public:
    OSystemFNGO() = default;
    ~OSystemFNGO() override = default;

    // StellaHost installs this to learn about state transitions.
    std::function<void(EventHandlerState)> onStateChanged;

    void stateChanged(EventHandlerState state) override
    {
      if(onStateChanged) onStateChanged(state);
    }

    shared_ptr<KeyValueRepository> getSettingsRepository() override
    {
      return std::make_shared<KeyValueRepositoryJsonFile>(
        FSNode(FNGOHostHooks::baseDir() + "stella-settings.json"));
    }

    // Per-ROM property overrides are not persisted: the session re-applies
    // the user's controller choices itself, and Stella's built-in property
    // database covers the rest. (The JSON adapter needs an atomic
    // repository, which the JSON file store is not.)
    shared_ptr<CompositeKeyValueRepository> getPropertyRepository() override
    {
      return std::make_shared<CompositeKeyValueRepositoryNoop>();
    }

    shared_ptr<CompositeKeyValueRepositoryAtomic> getHighscoreRepository() override
    {
      return std::make_shared<CompositeKeyValueRepositoryNoop>();
    }

  protected:
    void getBaseDirectories(string& basedir, string& homedir,
                            bool, string_view) override
    {
      basedir = homedir = FNGOHostHooks::baseDir();
      if(basedir.empty())
        basedir = homedir = string(".") + FSNode::PATH_SEPARATOR;
    }

    void initPersistence(FSNode&) override { }
    string describePersistence() override { return "json files"; }

  private:
    OSystemFNGO(const OSystemFNGO&) = delete;
    OSystemFNGO(OSystemFNGO&&) = delete;
    OSystemFNGO& operator=(const OSystemFNGO&) = delete;
    OSystemFNGO& operator=(OSystemFNGO&&) = delete;
};

#endif // OSYSTEM_FNGO_HXX
