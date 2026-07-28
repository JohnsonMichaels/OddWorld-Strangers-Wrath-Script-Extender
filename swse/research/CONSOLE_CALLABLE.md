# SWSE - Functions Callable Through the Console

From 348 script functions with decoded signatures. `ID` = the ScriptContext SWSE already captures; scalars (int/bool/float/enum) we can pass directly.


## Callable NOW (181) - context + scalar args only

These need only the captured context and simple numbers - wire any of them into a console command immediately.

| function | handler | args |
|---|---|---|
| `ActivateAmbientEmitter` | 0x5580F0 | - |
| `ActivateGroup` | 0x55F5D0 | - |
| `ActivateSetGroup` | 0x55F680 | - |
| `ActivateVolume` | 0x55F300 | - |
| `AddStat` | 0x563C40 | int |
| `BeginCinematic` | 0x558750 | - |
| `BeginCinematicNoBars` | 0x5587F0 | - |
| `BeginCinematicRollControl` | 0x558780 | bool |
| `BeginJobLock` | 0x56A730 | - |
| `BoatHasTurret` | 0x55E020 | bool |
| `BoatMove` | 0x55E3B0 | - |
| `BoatMoveAboard` | 0x55DF50 | - |
| `BoatMoveAboardWF` | 0x55DF80 | - |
| `BoatMoveWF` | 0x55E450 | - |
| `BoatReleaseFromPath` | 0x55E3E0 | - |
| `BoatStartPhysics` | 0x55E5A0 | - |
| `BoatStopAutoRowing` | 0x55E610 | - |
| `BoatStopPhysics` | 0x55E480 | - |
| `BoatTakeTurret` | 0x55E810 | - |
| `BoatTeleportAboard` | 0x55DD00 | - |
| `BoatTeleportAshore` | 0x55DD30 | EMotionTeleport |
| `BoatTurretFireStart` | 0x553BD0 | - |
| `BoatTurretFireStop` | 0x553CA0 | - |
| `BoatWF` | 0x55E680 | - |
| `BountyPost_ClearAssignments` | 0x562360 | - |
| `BountyPost_ClearPaidAssignments` | 0x5623A0 | - |
| `CineFX_SetFadeIn` | 0x5609E0 | float |
| `ClearActiveBolts` | 0x5612F0 | - |
| `ClearAllBolts` | 0x561330 | - |
| `CombatGotoWF` | 0x56E660 | - |
| `ConversationWaitFor` | 0x55F110 | - |
| `CountNPCAlive` | 0x55D240 | int |
| `CountNPCAliveInGroup` | 0x55D490 | int |
| `CountNPCAliveOfType` | 0x55D430 | int |
| `CountNPCAliveOfTypeInGroup` | 0x55D680 | int |
| `CountNPCDead` | 0x55D1F0 | int |
| `CountNPCInAgit` | 0x55D2E0 | int |
| `CountNPCInAgitInGroup` | 0x55D530 | int |
| `CountNPCInCombat` | 0x55D330 | int |
| `CountNPCInCombatInGroup` | 0x55D580 | int |
| `CountNPCInNormal` | 0x55D290 | int |
| `CountNPCInNormalInGroup` | 0x55D4E0 | int |
| `CountNPCInPanic` | 0x55D380 | int |
| `CountNPCInPanicInGroup` | 0x55D5D0 | int |
| `CountNPCOfType` | 0x55D3D0 | int |
| `CountNPCOfTypeInGroup` | 0x55D620 | int |
| `CreateCineGroupFromVolume` | 0x55D740 | - |
| `CrossbowWeaponOnLeft` | 0x561880 | - |
| `CrossbowWeaponOnRight` | 0x561930 | - |
| `CurrentSetEnabled` | 0x55E880 | - |
| `CutTo` | 0x559B90 | - |
| `DeactivateAmbientEmitter` | 0x5581B0 | - |
| `DeactivateGroup` | 0x55F730 | - |
| `DeactivateVolume` | 0x55F330 | - |
| `DeleteZone` | 0x550570 | - |
| `DevicePressWF` | 0x55FCF0 | - |
| `DisableCombatMusic` | 0x556710 | - |
| `DisableSaveLoad` | 0x55C950 | - |
| `DisableTensionMusic` | 0x556690 | - |
| `DoPlayerVictoryWF` | 0x571DD0 | - |
| `DontFixFeet` | 0x572270 | - |
| `EnableCombatMusic` | 0x5566D0 | - |
| `EnableSaveLoad` | 0x55C910 | - |
| `EnableTensionMusic` | 0x556650 | - |
| `EndCinematic` | 0x558820 | - |
| `EndGame` | 0x5606B0 | - |
| `EndJobLock` | 0x56A980 | - |
| `FadeIn` | 0x558AE0 | float |
| `FadeInFromColor` | 0x558BA0 | float, int |
| `FadeInFromColorWF` | 0x558BD0 | float, int |
| `FadeInWF` | 0x558B10 | float |
| `FadeOut` | 0x558B40 | float |
| `FadeOutToColor` | 0x558C00 | float, int |
| `FadeOutToColorWF` | 0x558C30 | float, int |
| `FadeOutWF` | 0x558B70 | float |
| `FlushResourceManager` | 0x561D40 | - |
| `FollowWF` | 0x56E180 | - |
| `ForceCombat` | 0x552E90 | - |
| `ForceFPS` | 0x558470 | - |
| `ForceNotFPS` | 0x558490 | - |
| `ForceSniper` | 0x5584B0 | - |
| `GeneralStore_AddItemQuantity` | 0x562960 | int |
| `GeneralStore_SetItemQuantity` | 0x562A50 | int |
| `GetBool` | 0x54F370 | bool |
| `GetFloat` | 0x54F270 | float |
| `GetHealth` | 0x569240 | float |
| `GetID` | 0x54F8A0 | - |
| `GetInt` | 0x54F170 | int |
| `GetMindState` | 0x5696E0 | EMindState |
| `GetMoolah` | 0x5630E0 | float |
| `GetNameOfBountyAssignment` | 0x5637D0 | - |
| `GetStamina` | 0x569490 | float |
| `GiveDefaultAmmo` | 0x560E90 | - |
| `GotoRunWF` | 0x56F020 | - |
| `GotoSlowWF` | 0x56EB40 | - |
| `GotoWF` | 0x56DCA0 | - |
| `IdleWF` | 0x56F500 | - |
| `Inc` | 0x54F510 | - |
| `IsCinematicRunning` | 0x559160 | bool |
| `IsEnvironmentalCueActive` | 0x558290 | bool |
| `IsPlayerOnRope` | 0x54D710 | bool |
| `IsTownPanicking` | 0x55DC60 | bool |
| `IsVolumeActive` | 0x55F360 | bool |
| `KeepInMindState` | 0x569B80 | - |
| `KnockPlayerOffRope` | 0x54D780 | - |
| `LoadLastSave` | 0x5622A0 | - |
| `LockPlayerDisguise` | 0x54D430 | - |
| `Log` | 0x54C060 | - |
| `MakePlayerSteef` | 0x54D2F0 | - |
| `MakePlayerStranger` | 0x54D390 | - |
| `MoveIntoPurgatoryGroup` | 0x5603C0 | - |
| `MoveIntoPurgatoryUpdateNav` | 0x572730 | - |
| `MoveOutOfPurgatory` | 0x5600C0 | - |
| `MoveOutOfPurgatoryGroup` | 0x560330 | - |
| `MoveOutOfPurgatoryUpdateNav` | 0x5724D0 | - |
| `MoveTo` | 0x55A160 | float |
| `MoveToWF` | 0x55A190 | float |
| `NextCinematicPagingTracksCamera` | 0x5583B0 | - |
| `OpenWeaponHUD` | 0x5622F0 | - |
| `PlayRumble` | 0x55CC70 | - |
| `PlaySoundUI` | 0x556840 | - |
| `PlaySpeechAtWF` | 0x5576F0 | - |
| `PlaySpeechVO` | 0x557CF0 | - |
| `PlayerHasAmmo` | 0x5619E0 | bool |
| `PlayerInBoat` | 0x55DFB0 | bool |
| `Player_GetBountyAssignmentFinished` | 0x562610 | bool |
| `Player_GetBountyAssignmentFinishedKilled` | 0x5626D0 | bool |
| `Player_SetBountyAssignmentFinished` | 0x5628B0 | bool |
| `PopMusic` | 0x556590 | - |
| `PostAlarm` | 0x54CE10 | - |
| `PrimeCue` | 0x556CF0 | - |
| `PrimeSpeechCue` | 0x556D80 | - |
| `PushMusic` | 0x5564E0 | EMusicType |
| `QuickSave` | 0x54D8A0 | - |
| `RandomFloat` | 0x54C370 | float |
| `RandomInt` | 0x54C2F0 | int |
| `Reboot` | 0x54D860 | - |
| `ReturnToGameCam` | 0x55A460 | - |
| `ReturnToGameCamSpecific` | 0x55A490 | float |
| `SayWF` | 0x5703A0 | - |
| `Set` | 0x54F470 | float |
| `SetCinematicSkippable` | 0x5586B0 | bool |
| `SetCritterPathSpawnEnabled` | 0x5616D0 | - |
| `SetDOFDistance` | 0x55EF40 | float |
| `SetDOFRange` | 0x55EE60 | float |
| `SetDesiredMechState` | 0x55FBC0 | - |
| `SetDesiredMechStateWF` | 0x55FC10 | - |
| `SetFullRunAllowed` | 0x561380 | bool |
| `SetJournalText` | 0x563820 | int |
| `SetMindState` | 0x569930 | EMindState |
| `SetMoolah` | 0x563140 | float |
| `SetRepeaterCue` | 0x556750 | - |
| `SetReverbEnvironment` | 0x5564A0 | - |
| `SetSteefNaked` | 0x54D820 | - |
| `ShowHealthBars` | 0x558710 | - |
| `StartCountdownTimer` | 0x5621B0 | int |
| `StartEnvironmentalCue` | 0x558210 | - |
| `StartGS` | 0x572990 | - |
| `StopCountdownTimer` | 0x562230 | - |
| `StopEnvironmentalCue` | 0x558330 | - |
| `TakeAllAmmo` | 0x560B90 | - |
| `TakeAllArtifacts` | 0x5634D0 | - |
| `TakeAllWeapons` | 0x560A80 | - |
| `TeleportHome` | 0x54E2C0 | - |
| `TextOverlay` | 0x561D80 | - |
| `TextOverlayClear` | 0x562080 | - |
| `TextOverlayClose` | 0x562040 | - |
| `TextOverlayOpen` | 0x561FA0 | int |
| `TextOverlayPause` | 0x561DF0 | - |
| `TextOverlayUpDuringGUI` | 0x5620C0 | - |
| `TransitionMusic` | 0x5565D0 | EMusicType |
| `TriggerAchievement` | 0x563B90 | int |
| `UnlockAllMovies` | 0x54D550 | - |
| `UnlockPlayerDisguise` | 0x54D4C0 | - |
| `UpdateJournal` | 0x5638F0 | - |
| `UpdateNav` | 0x569DD0 | - |
| `UpdateNavObject` | 0x56A030 | - |
| `WaitForButton` | 0x54C590 | GameButton |
| `WaitForZonePagedIn` | 0x550360 | - |
| `float` | 0x682260 | float, int |
| `not` | 0x683040 | bool, bool |

## Needs arg-marshalling (124) - Object/String/Pref args

Callable once SWSE can marshal a string/object arg (e.g. GiveAmmo with an ammo .txt name, Teleport with tag names).

| function | handler | args |
|---|---|---|
| `ActivateDuration` | 0x55F890 | Object |
| `ActivateEffect` | 0x55C790 | Object |
| `ActivateFromActivator` | 0x55F980 | Object |
| `ActivateSet` | 0x55F450 | Object |
| `AddObjectToGroup` | 0x55D990 | bool, Object |
| `ArtifactPref` | 0x568E50 | ArtifactPref, String |
| `AttachGeometry` | 0x55C3B0 | Geometry, Object, Resource |
| `BoatGiveTurret` | 0x55E760 | MechanicalPref |
| `BoatPlayAnim` | 0x55E090 | Resource |
| `BountyPost_AddAssignment` | 0x5623E0 | BountyPref |
| `BountyPost_MakeAssignmentAvailable` | 0x562490 | BountyPref |
| `BountyPost_MakeAssignmentNotAvailable` | 0x562520 | BountyPref |
| `BountyPref` | 0x568D60 | BountyPref, String |
| `BreakLockToGround` | 0x54E070 | Object |
| `BroadcastPlayerNoise` | 0x54CEC0 | Object |
| `CombatGoto` | 0x56C540 | ShortGoal, Object |
| `DeactivateEffect` | 0x55C850 | Object |
| `DefaultJob` | 0x54CAA0 | Job |
| `Delete` | 0x54D900 | Object |
| `DestroyAttachedBolts` | 0x54D640 | Object |
| `DetachGeometry` | 0x55C630 | Object |
| `DisableShowingOnRadar` | 0x5616A0 | Object |
| `EffectPref` | 0x568E00 | EffectPref, String |
| `EnableShowingOnRadar` | 0x561670 | Object |
| `EndCineTorsoAnim` | 0x56AE20 | Object |
| `ExplosionPref` | 0x568EF0 | ExplosionPref, String |
| `FireBolt` | 0x553850 | NPCWeaponPrefs, String |
| `FireBoltMiss` | 0x5538B0 | NPCWeaponPrefs, String |
| `FireWeaponNoReload` | 0x56C9E0 | ShortGoal, Object, int |
| `GetWorker` | 0x56A290 | Object |
| `GiveAmmo` | 0x5610F0 | String |
| `GiveAmmoForceDowngrade` | 0x561120 | String |
| `GiveArtifact` | 0x563560 | ArtifactPref |
| `GiveCrossbow` | 0x561770 | WeaponPref |
| `GoThroughDoorFast` | 0x56D7C0 | ShortGoal, Object, Object |
| `GoThroughDoorFastWF` | 0x571240 | ShortGoal, Object |
| `GoThroughDoorSlow` | 0x56D570 | ShortGoal, Object, Object |
| `GoThroughDoorSlowWF` | 0x570D60 | ShortGoal, Object |
| `GotoPlayerAggressive` | 0x56B760 | ShortGoal, Object |
| `GotoRun` | 0x56BC00 | ShortGoal, Object |
| `GotoSlow` | 0x56B9B0 | ShortGoal, Object |
| `GotoTurnWorkWF` | 0x570880 | Object, ShortGoal |
| `HasArtifactCount` | 0x563210 | int, ArtifactPref |
| `HireActor` | 0x559580 | Object, Object |
| `ImmobilizeSpiderBolaInstant` | 0x5538E0 | Object |
| `IsActive` | 0x55F7E0 | bool, Object |
| `IsInFOV` | 0x55EA90 | bool, Object |
| `IsInLOS` | 0x55EC00 | bool, Object |
| `IsInView` | 0x55ECD0 | bool, Object |
| `IsNull` | 0x54D020 | bool, Object |
| `IsPortalEnabled` | 0x5502F0 | bool, String |
| `IsSteef` | 0x54D110 | bool, Object |
| `IsStranger` | 0x54D200 | bool, Object |
| `IsZonePagedIn` | 0x550480 | bool, String |
| `KillObject` | 0x555380 | Object |
| `LevelTransition` | 0x560510 | String |
| `LoadLevel` | 0x560700 | String |
| `LockToGround` | 0x54DF90 | Object |
| `MechanicalPref` | 0x568EA0 | MechanicalPref, String |
| `MoveIntoPurgatory` | 0x5601E0 | Object |
| `NPCWeaponPrefs` | 0x568F90 | NPCWeaponPrefs, String |
| `PlayAnim` | 0x56CE80 | ShortGoal, Object |
| `PlayAnimBlend` | 0x56D0D0 | ShortGoal, Object, float |
| `PlayAnimBlendHold` | 0x56D320 | ShortGoal, Object, float |
| `PlayAnimWF` | 0x56F9E0 | Resource |
| `PlayBinkMovie` | 0x560900 | String |
| `PlayBinkMovie_CineSkip` | 0x560970 | String |
| `PlayEffect` | 0x55A7C0 | Object |
| `PlaySound` | 0x556A60 | SoundHandle |
| `PlaySoundOnBone` | 0x556B50 | SoundHandle |
| `PlaySpeech` | 0x556E90 | SoundHandle |
| `PlaySpeechAt` | 0x557280 | SoundHandle |
| `Player_GetBountyAssignment` | 0x5625B0 | String |
| `Player_SetBountyAssignment` | 0x562780 | BountyPref |
| `PortalDisable` | 0x550270 | String |
| `PortalEnable` | 0x5501F0 | String |
| `PutCurrentAnimationAtEnd` | 0x55A720 | Object |
| `Queue` | 0x5556B0 | ShortGoal |
| `QuickSpeak` | 0x562DF0 | Object |
| `RemoveObjectFromGroup` | 0x55DAA0 | bool, Object |
| `RumblePref` | 0x568DB0 | RumblePref, String |
| `SetHealth` | 0x555290 | Object |
| `SetHomePosition` | 0x54E380 | Object |
| `SetNPCAffected` | 0x561430 | Object, bool |
| `SetNPCForceFieldEnable` | 0x563A00 | Object |
| `SetNPCHealthBarEnable` | 0x563AD0 | Object |
| `SetNPCResponds` | 0x561500 | Object, bool |
| `SetPlayerSeen` | 0x5563A0 | Object |
| `SetStamina` | 0x555440 | Object |
| `SetToInitialHealthAndStamina` | 0x555230 | Object |
| `SetToMaxHealth` | 0x5552F0 | Object |
| `SetToMaxHealthAndStamina` | 0x555260 | Object |
| `SetToMaxStamina` | 0x5554A0 | Object |
| `SpawnEffectAttachedToBone` | 0x55ADF0 | EffectPref, String |
| `SpawnEffectAttachedToBonePosition` | 0x55B030 | EffectPref, String |
| `SpawnEffectAttachedToObject` | 0x55B270 | EffectPref |
| `SpawnEffectAttachedToObjectPosition` | 0x55B440 | EffectPref |
| `SpawnTendrilFromBoneToBone` | 0x55BDE0 | Effect, EffectPref, Object |
| `SpawnTendrilFromBoneToObject` | 0x55BAE0 | Effect, EffectPref, Object |
| `SpawnTendrilFromRandomBoneToObject` | 0x55C130 | Effect, EffectPref, Object |
| `StartCineTorsoAnim` | 0x56ABD0 | Object, int |
| `StartEffect` | 0x55A9A0 | Effect, Object |
| `StartJobOnSteef` | 0x559730 | Resource |
| `StartOrContinueConversation` | 0x562F50 | Object |
| `StartScript` | 0x5598E0 | Script, Object |
| `StartSoundWF` | 0x557F20 | SoundHandle |
| `StopAnimation` | 0x55A680 | Object |
| `StopEffect` | 0x55AC20 | Effect |
| `StopEffectDelayed` | 0x55ACF0 | Effect |
| `StopLastCue` | 0x556C50 | Object |
| `StopSound` | 0x558090 | SoundHandle |
| `StopSpeech` | 0x557C00 | Object |
| `String` | 0x6822E0 | String, int |
| `TakeArtifact` | 0x563350 | ArtifactPref |
| `TakeDamage` | 0x555320 | Object |
| `TakeStamina` | 0x5554D0 | Object, float |
| `TakeWeapon` | 0x561150 | String |
| `TeleportReset` | 0x54E200 | Object, EMotionTeleport |
| `TriggerAnimationReverse` | 0x55A620 | Object |
| `TriggerAnimationWF` | 0x55A650 | Object |
| `TriggerExplosion` | 0x55CD30 | Object |
| `TurnToWF` | 0x56FEC0 | Object |
| `WaitFor` | 0x555590 | ShortGoal |
| `WeaponPref` | 0x568F40 | WeaponPref, String |

## Signature not recovered (43)

`Activate`, `CameraWF`, `Checkpoint`, `Deactivate`, `DebugHudPrint`, `Dec`, `Exhaust`, `FireWeapon`, `Follow`, `ForceJob`, `GetDOFDistance`, `GetThis`, `Gib`, `GiveAllAmmo`, `Goto`, `GotoTurnWork`, `ID`, `Idle`, `Kill`, `LockCamera`, `Object`, `PopBattleMusic`, `PopEventMusic`, `PopTrumpMusic`, `PushBattleMusic`, `PushEventMusic`, `PushTrumpMusic`, `RingBell`, `Say`, `SetSteefClothed`, `ShakeOffDamage`, `Sleep`, `SpawnTendrilTweenObjects`, `Teleport`, `Time`, `TriggerAnimation`, `TriggerAnimationReverseWF`, `TurnTo`, `WaitForCameraTrans`, `and`, `int`, `or`, `sin`