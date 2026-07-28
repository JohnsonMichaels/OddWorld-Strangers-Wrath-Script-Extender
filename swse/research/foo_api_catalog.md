# Stranger's Wrath - .foo Script API Catalog

Extracted from 10 level blockmaps. 149 distinct functions.


## Player / form / camera

| Function | Uses | Example arguments |
|---|---:|---|
| `StartJobOnSteef` | 120 | `"TUT_cine_spider.foo"` |
| `ReceiveSpeechFromPlayer` | 105 | `` |
| `OnSeePlayer` | 41 | `Object` |
| `LockCamera` | 32 | `ELockCamera_1stPerson` |
| `ForceJob` | 30 | `"Nav_01_3_Tag_outlawsniper_5", "Nav_01_3_Tag_jobnode_ey6"` |
| `StartJobonSteef` | 2 | `"oa_01_1_teleport.foo"` |
| `OnReceiveSpeechFromPlayer` | 2 | `` |
| `Player_SetBountyAssignmentFinished` | 1 | `true` |
| `Forcejob` | 1 | `"outlaw_03_2_Tag_outlawboss_fattymcboomboom_1","outlaw_03_2_Tag_job_14` |
| `MakePlayerSteef` | 1 | `` |
| `PlayerInBoat` | 1 | `` |

## Weapons / ammo / store

| Function | Uses | Example arguments |
|---|---:|---|
| `GeneralStore_SetItemQuantity` | 20 | `"town_03_1_Tag_store_1", "/data/prefs/artifacts/damagebeegun.txt", 950` |
| `GiveAmmo` | 16 | `"ImmobilizeSkunkBomb.txt",-1` |
| `OnAmmoHit` | 11 | `ID,bool` |
| `TakeAllAmmo` | 5 | `` |
| `GeneralStore_AddItemQuantity` | 3 | `"town_01_1_Tag_store_1", "/data/prefs/Artifacts/damagearmadillo.txt", ` |
| `OnExitStoreUI` | 3 | `` |
| `TakeAllWeapons` | 2 | `` |
| `OnWeaponHUDOpen` | 1 | `` |
| `OnWeaponHUDClose` | 1 | `` |
| `OnWeaponFire` | 1 | `bool` |
| `TakeDamage` | 1 | `"transition_02_Tag_Decorator_de_transiton02_townsfolk_wall1" ,11` |

## Movement / teleport

| Function | Uses | Example arguments |
|---|---:|---|
| `MoveOutOfPurgatory` | 267 | `"tutorial_Tag_Decorator_arrow_018"` |
| `MoveIntoPurgatory` | 205 | `"tutorial_Tag_Decorator_arrow_019"` |
| `MoveOutOfPurgatoryGroup` | 164 | `"tutorial_Group_1"` |
| `SpawnTendrilTweenObjects` | 100 | `"\data\prefs\effects\EffectMixDef\sekto_generator_power_01.txt", "shoc` |
| `MoveIntoPurgatoryGroup` | 72 | `"gate_reinforcements"` |
| `Teleport` | 27 | `"tutorial_Tag_outlawcutter_tutorial_1","tutorial_chipPunkCollapse1",EM` |
| `OnSpawnLast` | 17 | `` |
| `RemoveObjectFromGroup` | 14 | `ID(obj` |
| `TeleportHome` | 7 | `obj` |

## NPC / combat / life

| Function | Uses | Example arguments |
|---|---:|---|
| `Kill` | 600 | `obj` |
| `OnNPCEnter` | 362 | `Object` |
| `OnDeath` | 202 | `Object` |
| `OnBounty` | 166 | `Object` |
| `TriggerExplosion` | 154 | `"Nav_01_3_Tag_ExplosionLocation_01","\data\prefs\explosions\no_render_` |
| `OnCombat` | 122 | `Object` |
| `OnNPCExit` | 38 | `Object` |
| `OnAgit` | 20 | `Object` |
| `OnDamage` | 20 | `` |
| `GetHealth` | 20 | `"PackratPalooka"` |
| `KillObject` | 19 | `"nav_hunt_area_1_1_Tag_outlawcutter_1"` |
| `CountNPCAlive` | 15 | `"tutorial_Tag_scriptVolume_124"` |
| `OnRepeatedDeath` | 6 | `` |
| `ForceCombat` | 6 | `"trans_02_1_Tag_outlawmortar_1"` |
| `BountyPost_AddAssignment` | 4 | `"\data\prefs\Bounty\Region2_Bounty1.txt"` |
| `SetNPCHealthBarEnable` | 4 | `"Wolvark_06_3_Tag_gloktigi_1", true` |
| `OnExitBountyUI` | 3 | `` |
| `SetNPCAffected` | 3 | `"town_01_1_Tag_outlawboss_movieboss_1", "\data\prefs\weapons\Punch.txt` |
| `DisableCombatMusic` | 1 | `` |

## World / triggers / anim

| Function | Uses | Example arguments |
|---|---:|---|
| `OnDestructDone` | 539 | `` |
| `Activate` | 523 | `"Tag_Mechanical_tutorial_outlaw_doorb"` |
| `Deactivate` | 257 | `"town_01_1_Tag_radarlocation_2"` |
| `TriggerAnimation` | 210 | `"town_01_1_Tag_Decorator_town_maingate_012",1` |
| `OnActivate` | 166 | `` |
| `ActivateVolume` | 122 | `"town_01_1_Tag_pageHintVolume_3"` |
| `DeactivateVolume` | 117 | `"outlaw_01_1_Tag_fuzscriptVolume_2"` |
| `OnDestructChange` | 38 | `` |
| `ActivateAmbientEmitter` | 35 | `"boss_01_1_Tag_audiolocation_1"` |
| `OnDeactivate` | 32 | `` |
| `DeactivateAmbientEmitter` | 31 | `"tutorial_Tag_audiolocation_1"` |
| `DeactivateGroup` | 19 | `"nav_01_1_EmptyJobsGroup"` |
| `ActivateGroup` | 5 | `"outlaw_radar_nodes"` |
| `ActivateEffect` | 1 | `"Native_02_1_Tag_effect_1"` |
| `TriggerAchievement` | 1 | `28` |

## UI / speech / journal

| Function | Uses | Example arguments |
|---|---:|---|
| `QuickSpeak` | 91 | `"Player1", "Steef_speak_Outlaw_01_3_selfGetLootenDuke_01"` |
| `PlaySound` | 78 | `EChannel_eChannelAmbient, "Outlaw_speak_Outlaw_01_3_intoPosition_s01",` |
| `QuickSave` | 67 | `` |
| `PlaySpeechVO` | 64 | `"Outlaw_speak_Tutorial_NabBountyHunter_01"` |
| `SetJournalText` | 63 | `0, "journal_region00_bola"` |
| `TextOverlay` | 52 | `"newTutorial_chipPunk_bounty"` |
| `PlaySpeech` | 51 | `"Steef_speak_Tutorial_Self_GetToTown_02","player1"` |
| `PlaySoundUI` | 39 | `"townsfolk_speak_nav_t02_1_panic02"` |
| `TextOverlayClose` | 38 | `` |
| `GetInsultCue` | 36 | `Object` |
| `GetInsultResponseCue` | 36 | `Object` |
| `PlayRumble` | 20 | `"trans_01_1_Tag_effect_19", "\data\prefs\Rumble\cin_earthquake.txt"` |
| `TextOverlayPause` | 14 | `1,"newTutorial_hideZone"` |
| `TextOverlayClear` | 14 | `` |
| `TextOverlayOpen` | 12 | `1,"newTutorial_reloadButton"` |
| `DefaultGameSpeak` | 9 | `` |
| `UpdateJournal` | 9 | `` |
| `DebugHudPrint` | 7 | `"Quicksaved", 3` |
| `StopLastCue` | 6 | `"boss_fight_tests_Tag_Decorator_de_bossFight_cartTransThree1"` |
| `StartEnvironmentalCue` | 5 | `"amb_rainHeavy_loop"` |
| `StopEnvironmentalCue` | 4 | `"amb_rainHeavy_loop"` |
| `Quicksave` | 3 | `` |
| `StopSpeech` | 3 | `"trans_02_1_Tag_townsfolk_2"` |
| `StopSound` | 3 | `handle` |
| `TextOverlayUpDuringGUI` | 2 | `` |
| `OnUpdateJournal` | 2 | `` |
| `TextoverlayClear` | 1 | `` |
| `PlaySoundOnBone` | 1 | `"fx_minecart_startloop","outlaw_03_3_Tag__FollowREINFORE011", "followR` |
| `SoundCue` | 1 | `int` |
| `PlaySpeechAt` | 1 | `"wolvark_speak_wolvark_03_1_pa_intruder04", "Wolvark_03_2_Tag_namedloc` |

## Query (Get*/Count*)

| Function | Uses | Example arguments |
|---|---:|---|
| `GetBool` | 663 | `"TUT_taughtSwim"` |
| `GetInt` | 195 | `"TUT_numCritters"` |
| `IsTownPanicking` | 6 | `"town_01_1_Tag_townpaniccontroller_1"` |
| `IsInFOV` | 3 | `"wolvark_04_2_proxydupe_107"` |
| `GetStamina` | 1 | `"PackratPalooka"` |
| `Getbool` | 1 | `"TN3_SkycartJoe2"` |
| `GetMoolah` | 1 | `` |
| `HasArtifactCount` | 1 | `"/data/prefs/Artifacts/mongoriverpass.txt"` |

## Other

| Function | Uses | Example arguments |
|---|---:|---|
| `OnEnter` | 2052 | `Object` |
| `Set` | 1464 | `"selfSpeak", 0` |
| `OnExit` | 472 | `Object` |
| `StartScript` | 175 | `"playerFellChasm"` |
| `OnActionButton` | 122 | `` |
| `OnExhaust` | 102 | `Object` |
| `Inc` | 91 | `"OA1_3_GotDuke"` |
| `Event_eAttack` | 70 | `` |
| `OnNormal` | 31 | `Object` |
| `Dec` | 21 | `"N1_iSlogsAlive"` |
| `BreakLockToGround` | 20 | `"player1"` |
| `Main` | 18 | `` |
| `LockToGround` | 11 | `"followREINFORCE"` |
| `OnBoatEnter` | 10 | `Object` |
| `PostLoad` | 9 | `` |
| `Checkpoint` | 9 | `"Checkpoint_Node_01"` |
| `RandomInt` | 9 | `10` |
| `Sleep` | 8 | `0.1` |
| `PopMusic` | 7 | `` |
| `OnCollect` | 6 | `ID` |
| `PushMusic` | 6 | `EMusicType_Base0` |
| `SetDesiredMechState` | 6 | `"area03_3ArrowLight_5", 1` |
| `PostLoadDebugDirectLoad` | 5 | `` |
| `DisableShowingOnRadar` | 5 | `"outlaw_02_2_Tag_radarlocation_bottom_3"` |
| `OnTick` | 5 | `float,float` |
| `Say` | 5 | `"town_01_1_Tag_townsfolkstorekeeper_1", "Townsfolk_speak_Town_03_1_Bou` |
| `PostLoadFirstTime` | 4 | `` |
| `EnableShowingOnRadar` | 4 | `"outlaw_02_2_Tag_radarlocation_bottom_1"` |
| `ForceNotFPS` | 4 | `` |
| `CheckPoint` | 3 | `"level_02_Tag_namedlocation_64"` |
| `OnPanic` | 3 | `` |
| `DontFixFeet` | 3 | `"native_04_1_DropshipGroup_1"` |
| `OnCritterHit` | 2 | `ID` |
| `PortalDisable` | 2 | `"tutorial_Portal_1"` |
| `OnCountdownExpire` | 2 | `` |
| `Log` | 2 | `(tempVar` |
| `AddObjectToGroup` | 2 | `obj, "outlaw_03_2_McBoomboomPostCinGuys"` |
| `OnBoatExit` | 2 | `Object` |
| `SetFullRunAllowed` | 1 | `true` |
| `DoGotoTurnWork` | 1 | `` |
| `OnGib` | 1 | `` |
| `CurrentSetEnabled` | 1 | `true` |
| `ElevatorDrop` | 1 | `` |
| `DisableTensionMusic` | 1 | `` |
| `TurnTo` | 1 | `"outlaw_03_2_Tag_outlawsniper_3", "player1"` |
| `StartCountdownTimer` | 1 | `180` |

## Ammo / item .txt names referenced

`ImmobilizeSpiderBola.txt`, `SendToChipmunk.txt`, `TrapFuzzle.txt`, `ActivateHiveQueen.txt`, `ImmobilizeSkunkBomb.txt`


## Common enum constants

`EMotionTeleport_StickToFloor`, `ELockCamera_Off`, `EChannel_eChannelMisc`, `EChannel_eChannelAmbient`, `EMotionTeleport_NoCollision`, `ELockCamera_1stPerson`, `ELockCamera_3rdPerson`, `EMusicType_Base2`, `ELockCamera_off`, `EMusicType_Base0`, `EMusicType_Battle7`