# Stranger's Wrath - Full Reflection Schema (SWSE feature map)

Auto-extracted from stranger.exe. **2014 reflected fields** across **106 class groups**, plus 181 enum constants. Grouping is heuristic (nearest class-like token before each field run) so treat boundaries as approximate.

> Fields = tunable/settable data (health, speeds, timers, damage, counts...). Combined with the 348 script functions (see script_handlers.tsv) this is the full SWSE surface.


## BoltSurfaceSndPrefs  (246 fields)

- `m_shakenBoltPrefsName` · `m_playerImmobilizeCount` · `m_immobilizeWrap`
- `m_immobilizeDuration` · `m_immobilizeRadius` · `m_killDistance`
- `m_knowShooterPos` · `m_lookAtPlayer` · `m_playerAmmoStrikeEventRangeBottom`
- `m_playerAmmoStrikeEventRangeTop` · `m_playerAmmoStrikeEventRangeXY` · `m_damageEventRangeBottom`
- `m_damageEventRangeTop` · `m_damageEventRangeXY` · `m_causesEvents`
- `m_bounceParameter` · `m_bounceOption` · `m_magnetizeIfNoTarget`
- `m_magnetismAllowTiltUp` · `m_magnetismLeadingScale` · `m_magnetismWhenPassed`
- `m_magnetismDelay` · `m_magnetismAfterDuration` · `m_magnetismDuration`
- `m_magnetismOverride` · `m_magnetismOption` · `m_driveAwaySpeedMultiplierStuck`
- `m_driveAwaySpeedMultiplier` · `m_drivesNPCsAway` · `m_delayForDuration`
- `m_outOfRangeDuration` · `m_durationInActor` · `m_duration`
- `m_stickEffectActor` · `m_stickEffect` · `m_hitWorldOptions`
- `m_hitShieldOptions` · `m_hitActorOptions` · `m_totalCapacity`
- `m_clipCapacity` · `m_clipCapacityMin` · `m_fallOffUnderwater`
- `m_outOfRangeDoEvents` · `m_outOfRangeFadeTime` · `m_outOfRangeDoFade`
- `m_outOfRangeDeliverPayload` · `m_falloffToRange` · `m_forceHurtReaction`
- `m_useGenericDamage` · `m_knocksOutOfFirstPersonFromArea` · `m_knockUsePleaseGetOut`
- `m_knockEfficiency` · `m_maxKnockSpeedPlayer` · `m_maxKnockSpeed`
- `m_areaHurtsShooter` · `m_runAwayRangeOffset` · `m_areaOfEffect`
- `m_explosionFriendAffectMultiplier` · `m_explosionSpeed` · `m_explosionDuration`
- `m_stuckDrainStamina` · `m_stuckDrainDamage` · `m_stuckDrainInterval`
- `m_continueIfBreakDestructable` · `m_canSetOffExplosives` · `m_attachedDamager`
- `m_gibOnMassiveDamage` · `m_gibOnKill` · `m_maxLeadVel`
- `m_pitchClamp` · `m_shakeOffZOffset` · `m_hitscanBlastAngle`
- `m_orientToVelocityInFlight` · `m_xyOnly` · `m_gravity2`
- `m_speed2` · `m_gravity` · `m_speed`
- `m_useHighArc` · `m_useArcTweaks` · `m_arcTime2`
- `m_arcDist2` · `m_arcAngle2` · `m_arcTime`
- `m_arcDist` · `m_arcAngle` · `m_effectPref`
- `m_speedRandomness` · `m_spiralTightness` · `m_spiralCenteredness`
- `m_offsetRandMax` · `m_offsetRandMin` · `m_initialDistribution`
- `m_initalRadius` · `m_finalDistribution` · `m_rotateSteppedShotDegrees`
- `m_finalSpreadVerticalScale` · `m_finalSpreadAngle` · `m_fireFirstRampShotStraight`
- `m_fireStraightAhead` · `m_fxImpactDamagePrefBase` · `m_fxImpactStickPrefBase`
- `m_fxImpactRicochetPrefBase` · `m_sndImpactStickBase` · `m_sndRicocheteBase`
- `m_spinDeadzone` · `m_spinRate` · `m_rmbFade`
- `m_rmbPreFadeFinish` · `m_rmbPreFadeIntermittent` · `m_rmbImpactStick`
- `m_rmbRicochete` · `m_rmbWithAmmoWhenFired` · `m_rmbWithShooterWhenFired`
- `m_rmbWhileHeld` · `m_fireRumbleDuration` · `m_fireRumbleIntensity`
- `m_preFadeTime` · `m_fxSurfaceSetName` · `m_fxFireStretchWindIfHitscan`
- `m_fxWeapChargedPref` · `m_fxWeapChargeUpPref` · `m_fxWeapReloadPref`
- `m_fxFlightPref` · `m_fxFadePref` · `m_fxPreFadeFinishPref`
- `m_fxUntilPreFadeIntermittentPref` · `m_fxStuckPref` · `m_fxFireAttachPref`
- `m_fxFirePref` · `m_sndSurfaceSetName` · `m_sndSpeechReload`
- `m_sndSpeechRelease` · `m_sndSpeechFire` · `m_sndFade`
- `m_sndPreFadeFinish` · `m_sndPreFadeIntermittent` · `m_sndImpactStickAux`
- `m_sndRicocheteAux` · `m_sndWhileStuck` · `m_sndInAir`
- `m_sndClipReload` · `m_sndWhenFiredAux` · `m_sndWhenFired`
- `m_sndWhileHeldOver` · `m_sndWhileHeld` · `m_auxInterruptType`
- `m_outOfRangeDoEffects` · `m_fadeAnim` · `m_workAnim`
- `m_impactAnim` · `m_flightAnim` · `m_spreadName`
- `m_hitscanBlastSquibCount` · `m_trailFadeDuration` · `m_trailColor`
- `m_trailThickness` · `m_trailDuration` · `m_geoName`
- `m_suckToBoltLaunchAngleRandomness` · `m_suckToBoltLaunchAngle` · `m_suckToBoltGravity`
- `m_suckToBoltRadius` · `m_jumpAngleMax` · `m_jumpAngleMin`
- `m_jumpOffActorMidairExplosionDelayRandomness` · `m_jumpOffActorMidairExplosionDelay` · `m_jumpSpeedOverride`
- `m_jumpSpawnBoltsOnDeath` · `m_dieBoltPref` · `m_onWeapGeo`
- `m_hudIcon` · `m_critter` · `m_autoAimToExplosives`
- `m_autoAimToActivatables` · `m_autoAimToTurrets` · `m_autoAimToCritters`
- `m_autoAimToNPCs` · `m_reticleNoAmmo` · `m_reticleOtherInRange`
- `m_reticleSubjectInRange` · `m_reticleOutOfRange` · `m_reticlePixelRadius`
- `m_initialLoadout` · `m_kickRightRumblePeakRamped` · `m_kickRightRumblePeak`
- `m_kickLeftRumblePeakRamped` · `m_kickLeftRumblePeak` · `m_kickScreenRandomRotBleedRamped`
- `m_kickScreenRandomRotBleed` · `m_kickScreenRandomRotRangeRamped` · `m_kickScreenRandomRotRange`
- `m_kickScreenHorizHoldRamped` · `m_kickScreenHorizHold` · `m_kickScreenVertHoldRamped`
- `m_kickScreenVertHold` · `m_kickScreenHorizPeakRamped` · `m_kickScreenHorizPeak`
- `m_kickScreenVertPeakRamped` · `m_kickScreenVertPeak` · `m_kickTimeScaleRamped`
- `m_kickTimeScale` · `m_kickRampDelay` · `m_kickRampPower`
- `m_kickRampedSeparateUp` · `m_kickRamped` · `m_kickRampDownTime`
- `m_kickRampUpTime` · `m_kickSeparateUp` · `m_kick`
- `m_useArmAnimations` · `m_lowerTime` · `m_raiseTime`
- `m_chargeIsAnalog` · `m_chargeBoolSnapover` · `m_chargeTime`
- `m_breederBagRate` · `m_fireRateRampDownTime` · `m_fireRateRampUpCount`
- `m_fireRateMax` · `m_reloadTimeNormal` · `m_fireRateNormal`
- `m_shouldPreserveAmmoCount` · `m_upgradeLevel` · `m_quickLoadoutSlot`
- `m_fireType` · `m_prefsCharged` · `m_artifactPrefs`
- `m_descriptionLine2` · `m_description` · `m_activateDuration`
- `m_sendToDistanceHold` · `m_sendToDistanceBias` · `m_maxAffected`
- `m_viewRadius` · `m_callRadius` · `m_sndManyResponseBase`
- `m_sndAttackMany` · `m_sndAttackSolo` · `m_sndOof`
- `m_flopMultiplier` · `m_relaunchBoltPref` · `m_damagePerSecond`
- `m_attackDistance` · `m_agitDistance` · `m_attackCritter`

## PlayerPrefs  (179 fields)

- `m_exitSplashEffectName` · `m_deathTunnelEdgeThickness` · `m_deathTunnelResolution`
- `m_deathTunnelDuration` · `m_doDeathCamEffects` · `m_topFlashFadeAcceleration`
- `m_topFlashFadeVelocity` · `m_topFlashWidth` · `m_sideFlashFadeAcceleration`
- `m_sideFlashFadeVelocity` · `m_sideFlashWidth` · `m_collectTabBeginScaleFadeTime`
- `m_collectTabBeginScale` · `m_collectTabTexture` · `m_collectTabPresentTime`
- `m_collectTabFadeOutTime` · `m_collectTabFadeInTime` · `m_collectTabFadeOutSnd`
- `m_collectTabFadeInSnd` · `m_collectTabIconSizeY` · `m_collectTabIconSizeX`
- `m_collectTabIconOffsetY` · `m_collectTabIconOffsetX` · `m_collectTabTextBottomSize`
- `m_collectTabTextBottomOffsetY` · `m_collectTabTextBottomOffsetX` · `m_collectTabTextBottomColor`
- `m_collectTabTextTopFullSize` · `m_collectTabTextTopFullOffsetY` · `m_collectTabTextTopFullOffsetX`
- `m_collectTabTextTopFullColor` · `m_collectTabTextTopSize` · `m_collectTabTextTopOffsetY`
- `m_collectTabTextTopOffsetX` · `m_collectTabTextTopColor` · `m_collectTabTextJustification`
- `m_collectTabSlideDir` · `m_collectTabSizeY` · `m_collectTabSizeX`
- `m_collectTabOffsetFromEdge` · `m_collectTabOffset` · `m_collectableTabConfigTabSpacing`
- `m_collectableTabConfigMaxTabs` · `m_bountyRegisterBeginScaleFadeTime` · `m_bountyRegisterBeginScale`
- `m_bountyRegisterTexture` · `m_bountyRegisterPresentTime` · `m_bountyRegisterFadeOutTime`
- `m_bountyRegisterFadeInTime` · `m_bountyRegisterFadeOutSnd` · `m_bountyRegisterFadeInSnd`
- `m_bountyRegisterIconSizeY` · `m_bountyRegisterIconSizeX` · `m_bountyRegisterIconOffsetY`
- `m_bountyRegisterIconOffsetX` · `m_bountyRegisterTextBottomSize` · `m_bountyRegisterTextBottomOffsetY`
- `m_bountyRegisterTextBottomOffsetX` · `m_bountyRegisterTextBottomColor` · `m_bountyRegisterTextTopSize`
- `m_bountyRegisterTextTopOffsetY` · `m_bountyRegisterTextTopOffsetX` · `m_bountyRegisterTextJustification`
- `m_bountyRegisterTextTopColor` · `m_bountyRegisterSlideDir` · `m_bountyRegisterSizeY`
- `m_bountyRegisterSizeX` · `m_bountyRegisterOffsetFromEdge` · `m_bountyRegisterOffset`
- `m_sniperAmmoTextScale` · `m_sniperAmmoTextPosY` · `m_sniperAmmoTextPosX`
- `m_sniperMuteIconName` · `m_sniperMuteIconColor` · `m_sniperMuteIconHeight`
- `m_sniperMuteIconWidth` · `m_sniperMuteIconPosY` · `m_sniperMuteIconPosX`
- `m_sniperVolumeIconName` · `m_sniperVolumeIconColor` · `m_sniperVolumeIconHeight`
- `m_sniperVolumeIconWidth` · `m_sniperVolumeIconPosY` · `m_sniperVolumeIconPosX`
- `m_sniperAmmoIconColor` · `m_sniperAmmoIconHeight` · `m_sniperAmmoIconWidth`
- `m_sniperAmmoIconPosY` · `m_sniperAmmoIconPosX` · `m_punchSmackRayRadius`
- `m_punchUseFacingWithinAngle` · `m_punchHysterisis` · `m_punchKeepTargetMinRating`
- `m_punchFindTargetMinRating` · `m_damageArrowHeight` · `m_damageArrowWidth`
- `m_damageArrowRightMargin` · `m_damageArrowLeftMargin` · `m_damageArrowBottomMargin`
- `m_damageArrowTopMargin` · `m_damageArrowColor` · `m_damageArrowTextureName`
- `m_sniperToggleStaysInSniper` · `m_dropOutOfSniperSecondLevelIfHurt` · `m_dropOutOfSniperIfHurt`
- `m_forceThirdPersonInBounty` · `m_inactiveTextColor` · `m_activeTextColor`
- `m_inactiveIconColor` · `m_activeIconColor` · `m_quickHudTextureScaleY`
- `m_quickHudTextureScaleX` · `m_quickHudTextureY` · `m_quickHudTextureX`
- `m_quickHudTexture` · `m_quickHudInfoTextPosY` · `m_quickHudInfoTextPosX`
- `m_quickHudInfoTextColor` · `m_quickHudInfoTextSize` · `m_quickHudInfoText1`
- `m_quickHudSelectHilightHeight` · `m_quickHudSelectHilightWidth` · `m_quickHudGlowOffsetY`
- `m_quickHudGlowOffsetX` · `m_quickHudGlowSizeY` · `m_quickHudGlowSize`
- `m_quickHudSelectIconSize` · `m_quickHudMarkerOffsetY` · `m_quickHudMarkerOffsetX`
- `m_quickHudTextYOffsetForReturn` · `m_quickHudDescTextSizeSelected` · `m_quickHudDescTextSize`
- `m_quickHudDescTextOffsetY` · `m_quickHudDescTextOffsetX` · `m_quickHudTextOffsetY`
- `m_quickHudTextOffsetX` · `m_quickHudSizeRampTime` · `m_quickHudCycleTime`
- `m_quickHudSpacing` · `m_quickHudTopY` · `m_quickHudCenterX`
- `m_hudTextOffsetY` · `m_hudTextOffsetX` · `m_hudLargeTextSize`
- `m_hudSmallTextSize` · `m_activeIconSize` · `m_inactiveIconScale`
- `m_hudDamageSpinRate` · `m_hudSpecialRadius` · `m_hudDamageRadius`
- `m_hudCenterY` · `m_hudCenterX` · `m_iconSizeRampTime`
- `m_hudDisplayTime` · `m_hudAlphaFadeOutTime` · `m_hudAlphaFadeInTime`
- `m_maxDofDistanceIfNoCollision` · `m_dofDistanceLerpSpeed` · `m_dofUVOffset`
- `m_numDOFFilterPasses` · `m_zoomRayRadius` · `m_zoomMaxDist2`
- `m_zoomMaxDist1` · `m_dofRangeDistanceAdd` · `m_dofRange`
- `m_rmbDeath` · `m_rmbShakeOffThirdPerson` · `m_rmbShakeOffFirstPerson`
- `m_shakenBoltAverageSpeed` · `m_enduranceRegenMaxVel` · `m_enduranceRegenRate`
- `m_endurance_3rdPersonRightTrigger` · `m_endurance_3rdPersonLeftTrigger` · `m_endurance_ShakePerHealth`
- `m_shakeExtraDurationFraction` · `m_shakeWaitAfterLastDamage` · `m_shakeDuration`
- `m_stranger` · `m_beast`

## MovieListPrefs  (92 fields)

- `m_movieFileNames` · `m_largeWriteWarningPrefs` · `m_smallWriteWarningPrefs`
- `m_bossNameBackgroundName` · `m_bossNameBackgroundHeight` · `m_bossNameBackgroundWidth`
- `m_bossNameBackgroundTop` · `m_bossNameBackgroundLeft` · `m_multipleBossBarOffsetY`
- `m_multipleBossBarOffsetX` · `m_bossHealthBarRange` · `m_bossNameColor`
- `m_bossNameAlphaBlend` · `m_bossNameHeight` · `m_bossNameWidth`
- `m_bossNameTop` · `m_bossNameLeft` · `m_overlayTextureName`
- `m_radarTextureName` · `m_countdownBeepVolumeCurvePow` · `m_countdownBeepPeriod`
- `m_countdownWarningBeepThreshold` · `m_countdownBeepVolumeRisePeriod` · `m_countdownBeepMaxVolume`
- `m_countdownWarningFlashPeriod` · `m_countdownWarningTime` · `m_countdownTextName`
- `m_countdownIconName` · `m_countdownTextColor` · `m_countdownTextHeight`
- `m_countdownTextWidth` · `m_countdownTextY` · `m_countdownTextX`
- `m_countdownIconColor` · `m_countdownIconHeight` · `m_countdownIconWidth`
- `m_countdownIconY` · `m_countdownIconX` · `m_staminaIconName`
- `m_staminaIconColor` · `m_staminaIconHeight` · `m_staminaIconWidth`
- `m_staminaIconY` · `m_staminaIconX` · `m_spottedIconName`
- `m_spottedIconColor` · `m_spottedIconHeight` · `m_spottedIconWidth`
- `m_spottedIconY` · `m_spottedIconX` · `m_hideIconName`
- `m_hideIconColor` · `m_hideIconHeight` · `m_hideIconWidth`
- `m_hideIconY` · `m_hideIconX` · `m_bossStaminaBarAttributes`
- `m_bossHealthBarAttributes` · `m_healthBarAttributesInvincible` · `m_tagProgressBarAttributes`
- `m_staminaBarAttributes` · `m_healthBarAttributes` · `m_hideTextColor`
- `m_playerPointColor` · `m_normalConeColor` · `m_normalBlipColor`
- `m_combatConeColor` · `m_combatBlipColor` · `m_deadGuyColor`
- `m_playerSightConeColor` · `m_turretColor` · `m_inHideZoneRadarColor`
- `m_ringAroundTheRadarColor` · `m_radarColor` · `m_drawHideText`
- `m_drawSpottedIcon` · `m_drawHideIcon` · `m_drawRingAroundTheRadar`
- `m_hiddenTextPos` · `m_sightConeFadeFactor` · `m_radarFadeInDuration`
- `m_radarFadeOutDuration` · `m_playerPointSize` · `m_radarPointSize`
- `m_damageIconSize` · `m_damageFlashDuration` · `m_radarZoneTraversalLimit`
- `m_radarRadius` · `m_overlaySize` · `m_radarSize`
- `m_radarY` · `m_radarX`

## GPrefs  (76 fields)

- `m_mouseSensitivity` · `m_loadingScreenFadeTime` · `m_moviesLocked`
- `m_deathFadeDuration` · `m_deathWaitBeforeFade` · `m_maxDeadBodiesAllowed`
- `m_locDir` · `m_creditMovies` · `m_debugInitialAmmoTestLevel`
- `m_loadingBarFadeInFrames` · `m_loadingErrorWarningEnabled` · `m_loadingBarNumTextureLayers`
- `m_loadingBarHeight` · `m_loadingBarMinY` · `m_loadingBarWidth`
- `m_loadingBarMinX` · `m_loadingBarColor` · `m_loadingBarTextureRepeat`
- `m_loadingBarScrollSpeeds` · `m_I3DL2Enabled` · `m_isDVDRun`
- `m_lipSyncEnabled` · `m_SWFNominalGlyphPixelSize` · `m_doAudioIOThread`
- `m_playLoadingAnimation` · `m_weaponCycleScheme` · `m_hudTextShadowColor`
- `m_hudTextColor` · `m_camFlipFollowYaw` · `m_camFlipFollowPitch`
- `m_camFlipFPSPitch` · `m_logAI` · `m_logContent`
- `m_profilerMode` · `m_cheatInvincible` · `m_loadDebugGeo`
- `m_warningsAreErrors` · `m_logNoFileIO` · `m_logEnabled`
- `m_keyboardNotJournalled` · `m_pipeAutoAttach` · `m_autoAimStrength`
- `m_widescreen` · `m_rumbleEnabled` · `m_bootToLevelList`
- `m_initialHudCategory` · `m_hudEnabled` · `m_journalFile`
- `m_tryToStopJournal` · `m_loadJournal` · `m_saveJournal`
- `m_audioHeadphonesEnabled` · `m_globalVoiceOverVolume` · `m_audioMusicVolume`
- `m_audioMasterVolume` · `m_audioLogFile` · `m_audioVerbose`
- `m_musicEnabled` · `m_audioEnabled` · `m_effectsEnabled`
- `m_boltsEnabled` · `m_startCines` · `m_makeNPCs`
- `m_fileToEcho` · `m_allowCinematicsToBeSkippedFirstTime` · `m_doSaveLoadForDeath`
- `m_doPauseGUI` · `m_doInventoryGUI` · `m_skipShellGUI`
- `m_onStartLoadLastSavegame` · `m_user` · `m_realLevelName`
- `m_defaultLevelFile` · `m_waitForTweakBeforeInitPeriod` · `m_numViewPanes`
- `m_numPlayers`

## XBowToHand  (69 fields)

- `m_townsfolkAnnoyedCue` · `m_flowMarkerPrefsFile` · `m_levelWaveBankOverride`
- `m_audioPrefsFile` · `m_lightmapperCreaseAngleDegrees` · `m_lightmapperTexelSize`
- `m_waterPrefsFile` · `m_skyPrefsFile` · `m_sunLightDirection`
- `m_sunLightColor` · `m_sunLightIntensity` · `m_sunLightNormalHardness`
- `m_pipeDebugView` · `m_startNaked` · `m_levelContainsNakedSteef`
- `m_canChangeDisguise` · `m_startDisguised` · `m_gameControl`
- `m_offset_rand` · `m_frequency_rand` · `m_amplitude_rand`
- `m_offset` · `m_frequency` · `m_amplitude`
- `m_amplitudeMax_rand` · `m_amplitudeMin_rand` · `m_amplitudeMax`
- `m_amplitudeMin` · `m_flip` · `m_holdEnd`
- `m_holdBegin` · `m_delay_rand` · `m_duration_rand`
- `m_power_rand` · `m_delay` · `m_power`
- `m_releaseLevel_rand` · `m_releaseDur_rand` · `m_sustainDur_rand`
- `m_sustainLevel_rand` · `m_decayDur_rand` · `m_attackPeakLevel_rand`
- `m_attackDur_rand` · `m_offsetDur_rand` · `m_offsetLevel_rand`
- `m_ADSR_global_rand` · `m_releaseLevel` · `m_releaseDur`
- `m_sustainDur` · `m_sustainLevel` · `m_decayDur`
- `m_attackPeakLevel` · `m_attackDur` · `m_offsetDur`
- `m_offsetLevel` · `m_interpolation` · `m_enabled`
- `m_scale` · `m_base` · `m_curve`
- `m_noise` · `m_sine` · `m_input_global_rand`
- `m_constant` · `m_inputType` · `m_inputs`
- `m_global_rand` · `m_peakLevel` · `m_envelope`

## GlobalMotionPrefs  (66 fields)

- `m_bouyancyMaxDepth` · `m_slideSideWaysSpeed` · `m_slideTerminalVelocity`
- `m_playerStrafeSpeed` · `m_anim_canterToRunBlendTime` · `m_turn_param_multiply`
- `m_turn_weight_thresh` · `m_turn_param_add` · `m_velMaxNoFreeMove`
- `m_velKillZRate` · `m_walkIsInCanterToRunBeforeEndTime` · `m_skidToStopBeforeEndTime`
- `m_skidToStopMinTime` · `m_walkTurnWeightScale` · `m_steef_PreTurnRadians`
- `m_turnAroundEaseOutDuration` · `m_turnAroundInitialWeight` · `m_turnAroundParameter`
- `m_turnAroundCos` · `m_walkDoSkidToStop` · `m_walkDoTurnAround`
- `m_fracCanterRunTransitionStranger` · `m_fracCanterRunTransitionBeast` · `m_fracTrotCanterTransition`
- `m_fracWalkTrotTransition` · `m_heightForNPCKnock` · `m_heightForSlam`
- `m_velocityToBounceOffWall` · `m_maxVelocityForBackwards` · `m_anim_walkInternalBlendTime`
- `m_anim_idleToWalkBlendTime` · `m_anim_jumpLandInitialWeight` · `m_turnTo1stPersonDuration_Pause`
- `m_turnTo1stPersonDuration_Opposing` · `m_turnTo1stPersonDuration_Facing` · `m_endurance_RamToStop`
- `m_bouyancyTime` · `m_timeToDrown` · `m_fallStaminaPerMeter`
- `m_fallStaminaBase` · `m_fallDamagePerMeter` · `m_fallDamageBase`
- `m_maxRamDamage` · `m_minRamDamage` · `m_npcRamDamage`
- `m_adjustVelocityVelDifferenceAt30fps` · `m_gravityForEnergy` · `m_gravityForFall`
- `m_velocityForFallFar` · `m_strafeSpeedScaleDecel` · `m_strafeSpeedScaleAccel`
- `m_strafeSidewaysFraction` · `m_strafeBackwardsFraction` · `m_knockbackElasticity`
- `m_knockbackCollisionVelocity` · `m_ramMinVel` · `m_ramElasticityMax`
- `m_ramElasticityMin` · `m_ramDamageDeadzoneFraction` · `m_knockNotRammedScale`
- `m_knockRammedScale` · `m_knockZRatioBoost` · `m_knockZRatioDest`
- `m_knockZRatioSource` · `m_airControlHi` · `m_airControlLo`

## NPCPrefs  (64 fields)

- `m_damageStarsOffset` · `m_rarePrefs` · `m_dratIncRatchetWhenHurt`
- `m_dratIncRatchetFriendHurt` · `m_dratRunToPlayer` · `m_fadeOutOnDeath`
- `m_aiLowDetail` · `m_aiHighDetail` · `m_defaultAttachments`
- `m_attachments` · `m_onStrangerRamDeadCollectableSpawner` · `m_onSteefRamDeadCollectableSpawner`
- `m_onStrangerRamAliveCollectableSpawner` · `m_onSteefRamAliveCollectableSpawner` · `m_onDeathCollectableSpawner`
- `m_onExhaustCollectableSpawner` · `m_onDamageCollectableSpawner` · `m_collectableSpawnLimit`
- `m_respondsToTrapFuzzle` · `m_respondsToImmobilizeSpiderBola` · `m_respondsToImmobilizeSkunkBomb`
- `m_respondsToDamageDynamite` · `m_affList` · `m_affGenerally`
- `m_spiderImmobilizeMultiplier` · `m_armadilloKnocks` · `m_armadilloDamageMultiplier`
- `m_fuzzleFleeRadius` · `m_fuzzleFlopFrequency` · `m_fuzzleShakeOffTime`
- `m_insultResponseCue` · `m_insultCue` · `m_replyStranger`
- `m_replySteef` · `m_steefSpeakCue` · `m_bountyEnduranceBoost`
- `m_bountyAmmoBoost` · `m_killMoolah` · `m_captureMoolah`
- `m_canBeBountied` · `m_rangedWeapon` · `m_meleeWeapon`
- `m_hurtReaction` · `m_autoAimRadius` · `m_onGibSpawnNPC`
- `m_allowOnDeathGibFromBolts` · `m_onDeathGib` · `m_exhaustedRecoverMultiplier`
- `m_exhaustTime` · `m_staminaRecoverTime` · `m_stamina`
- `m_healthRecoverTime` · `m_health` · `m_geoScaleMax`
- `m_geoScaleMin` · `m_instanceOverrides` · `m_bountyCategory`
- `m_audioName` · `m_waveBank` · `m_icon`
- `m_geometry` · `m_spAIPrefs` · `m_spMotionPrefs`
- `m_spActorPrefs`

## WeaponPrefs  (61 fields)

- `m_binocReticle` · `m_geoFrameOffsetY` · `m_geoFrameOffsetX`
- `m_ammoStages` · `m_quickAmmoList` · `m_quickHudFadeOutTimeAfterSelected`
- `m_quickHudFadeOutTimeAfterExpired` · `m_quickHudFadeInTime` · `m_quickHudDisplayTime`
- `m_reloadMinBackgroundAlpha` · `m_reloadMinTextAlpha` · `m_reloadGrowBackground`
- `m_reloadFadeBackground` · `m_ammoEmptyCursor` · `m_ammoEmptyCursorBlinkRate`
- `m_ammoInfinityScale` · `m_ammoRightInfinityY` · `m_ammoRightInfinityX`
- `m_ammoLeftInfinityY` · `m_ammoLeftInfinityX` · `m_ammoDisplayOffsetRightY`
- `m_ammoDisplayOffsetRightX` · `m_ammoDisplayOffsetLeftY` · `m_ammoDisplayOffsetLeftX`
- `m_ammoDisplayBackgroundEndOffsetY` · `m_ammoDisplayBackgroundEndOffsetX` · `m_ammoDisplayTextSpacing`
- `m_ammoDisplayBackgroundEmptyScaleY` · `m_ammoDisplayBackgroundEmptyScaleX` · `m_ammoDisplayBackgroundScaleY`
- `m_ammoDisplayBackgroundScaleX` · `m_ammoTextOffsetY` · `m_ammoTextOffsetX`
- `m_ammoTextScale` · `m_cycleTime` · `m_hudUseDampedDrive`
- `m_hudCursorRadius` · `m_hudCursorOffsetY` · `m_hudCursorOffsetX`
- `m_hudPreventSwitchToNext` · `m_hudDisplayTimeWhenCycled` · `m_hudSwitchTime`
- `m_hudAlphaFadeTimeWhenFired` · `m_hudAlphaFadeTime` · `m_peakMagnetismAngle`
- `m_peakMagnetismDist` · `m_maxAssistPixels` · `m_maxAdhesion`
- `m_maxFriction` · `m_rampTime` · `m_autoPunchEnabled`
- `m_altProjSpeed_HACK` · `m_sniperSpeedUp_HACK` · `m_primaryProjSpeed_HACK`
- `m_reloadFXRight` · `m_reloadFXLeft` · `m_initialSpecialLoadout`
- `m_initialDamageLoadout` · `m_animConfig` · `m_geomConfig`
- `m_weaponName`

## BoatPrefs  (50 fields)

- `m_oarSplashEffectName` · `m_velForDamageMax` · `m_velForDamageMin`
- `m_velForDamageNone` · `m_defaultTurret` · `m_driverOffset`
- `m_gunnerOffset` · `m_numIKIterations` · `m_singleStickReverseZone`
- `m_wakeSpawnMaxRateSpeed` · `m_wakeSpawnMaxRate` · `m_wakeSpawnMinRateSpeed`
- `m_wakeSpawnMinRate` · `m_oarCreakInterval` · `m_oarTipHeightOffset`
- `m_maxSyncMultiplier` · `m_minStickCloseness` · `m_decreaseOarSpeedTime`
- `m_increaseOarSpeedTime` · `m_blendFromIdleTime` · `m_blendToIdleTime`
- `m_sphereZOffset` · `m_aftSphereRadius` · `m_midSphereRadius`
- `m_foreSphereRadius` · `m_aftSphereOffset` · `m_foreSphereOffset`
- `m_buoyancyPontoonSinkDragConstant` · `m_buoyancyPontoonLiftDragConstant` · `m_buoyancyLatPontoonOffset`
- `m_buoyancyLongPontoonOffset` · `m_dragSensorLongOffset` · `m_dragSensorZOffset`
- `m_depthBumper` · `m_waterBoatDensityRatio` · `m_hullHeight`
- `m_perpTimeScale` · `m_perpTerminalVelocity` · `m_timeScale`
- `m_dragTurningEfficiency` · `m_inputMaxAccelRateFP` · `m_inputAccelSensitivityFP`
- `m_inputMaxTurnSpeedFP` · `m_inputTurnSensitivityFP` · `m_inputMaxAccelRate`
- `m_inputAccelSensitivity` · `m_inputMaxTurnSpeed` · `m_inputTurnSensitivity`
- `m_inputTerminalVelocityFP` · `m_terminalVelocity`

## CritterPrefs  (46 fields)

- `m_soundID` · `m_spawnEffectID` · `m_effectID`
- `m_poolIndexINTERNAL` · `m_critterTypeINTERNAL` · `m_onCrossbowTakeDamageIntervalMax`
- `m_onCrossbowTakeDamageIntervalMin` · `m_onCrossbowIdleIntervalMax` · `m_onCrossbowIdleIntervalMin`
- `m_stopRandomnessHigh` · `m_stopRandomnessNormal` · `m_stopFrequencyFrequent`
- `m_stopFrequencyNormal` · `m_stopFrequencyRare` · `m_spawnRandomnessHigh`
- `m_spawnRandomnessNormal` · `m_spawnFrequencyFrequent` · `m_spawnFrequencyNormal`
- `m_spawnFrequencyRare` · `m_motionSphereRadius` · `m_motionSphereHeight`
- `m_boltPref` · `m_type` · `m_geoScale`
- `m_pathDieAnim` · `m_pathWalkAnim` · `m_animationConfigFile`
- `m_fxImpactBase` · `m_fxImpactNonHealth` · `m_rmbImpactNonHealth`
- `m_useSuperDamageCue` · `m_sndImpactNonHealthSpeech` · `m_sndImpactNonHealth`
- `m_rmbImpact` · `m_sndImpact` · `m_rmbStart`
- `m_fxStartAir` · `m_fxStart` · `m_sndStartSpeech`
- `m_sndStart` · `m_range` · `m_knockbackVel`
- `m_exhaustedRecoverTime` · `m_damageScaleLowSpeed` · `m_damageDestructable`
- `m_damage`

## QuadAnimationControl  (45 fields)

- `m_gameControlZone` · `m_critterCuePrefs` · `m_shadowMapEndFade`
- `m_shadowMapStartFade` · `m_cloudShadowEndFade` · `m_cloudShadowStartFade`
- `m_surfaceSparkleColor` · `m_surfaceSparkleVerticalOffset` · `m_surfaceSparkleDensity`
- `m_surfaceSparkleRadius` · `m_surfaceSparkleFar` · `m_surfaceSparkleNear`
- `m_waterMurkColor` · `m_waterMurkMinStrength` · `m_waterMurkStrength`
- `m_battleMusicIndex` · `m_tensionMusicIndex` · `m_baseMusicIndex`
- `m_transFadeOutOverrideEnd` · `m_transFadeOutOverrideStart` · `m_glareEndFade`
- `m_glareStartFade` · `m_distortionScale` · `m_distantSingleLightViewColor`
- `m_cloudShadowSingleColorScale` · `m_cloudShadowTiling` · `m_cloudShadowSpeed`
- `m_cloudShadowRot` · `m_cloudShadowStrength` · `m_cloudShadowEnabled`
- `m_cloudShadowName` · `m_maxHunters` · `m_selfShadow`
- `m_ambientShadow` · `m_heightFogColor` · `m_heightFogHeight`
- `m_heightFogDepthToMaxFog` · `m_heightFogXYDistToMaxFog` · `m_lerpTime`
- `m_dofDistance` · `m_fogEnd` · `m_fogStart`
- `m_fogColor` · `m_reflectionMapName` · `m_allowFreeMovement`

## LaserBeam  (43 fields)

- `m_outerBeamShrinkWithTime` · `m_outerBeamGlare` · `m_outerBeamTextureScale`
- `m_outerBeamRotationRateDegrees` · `m_outerBeamRadiusScale` · `m_outerBeamScrollSpeed`
- `m_outerBeamTextureName` · `m_outerBeamColor_EndFire` · `m_outerBeamColor_MidFire`
- `m_outerBeamColor_StartFire` · `m_midBeamShrinkWithTime` · `m_midBeamGlare`
- `m_midBeamTextureScale` · `m_midBeamRotationRateDegrees` · `m_midBeamRadiusScale`
- `m_midBeamScrollSpeed` · `m_midBeamTextureName` · `m_midBeamColor_EndFire`
- `m_midBeamColor_MidFire` · `m_midBeamColor_StartFire` · `m_innerBeamShrinkWithTime`
- `m_innerBeamGlare` · `m_innerBeamTextureScale` · `m_innerBeamRotationRateDegrees`
- `m_innerBeamRadiusScale` · `m_innerBeamScrollSpeed` · `m_innerBeamTextureName`
- `m_innerBeamColor_EndFire` · `m_innerBeamColor_MidFire` · `m_innerBeamColor_StartFire`
- `m_numRadialFXPositions` · `m_laserFiringSource_GlareFXPref` · `m_laserFiringTarget_FXRadiusScale`
- `m_laserFiringTarget_FXDegreesBackwards` · `m_laserFiringTarget_FXUseRadialPositions` · `m_laserFiringTarget_FXPref`
- `m_laserFiringSource_FXRadiusScale` · `m_laserFiringSource_FXDegreesForwards` · `m_laserFiringSource_FXUseRadialPositions`
- `m_laserFiringSource_FXPref` · `m_laserChargingSource_FXRadiusScale` · `m_laserChargingSource_FXUseRadialPositions`
- `m_laserChargingSource_FXPref`

## AVEventPrefs  (40 fields)

- `m_eSawDeadPlayer` · `m_eExplosion` · `m_eStruggle`
- `m_eForceJob` · `m_eReset` · `m_eBreak`
- `m_eNPCShout` · `m_ePanic` · `m_eSawSteefChange`
- `m_eOnCompanyProperty` · `m_eTalkRequest` · `m_ePlayerNoise`
- `m_eTownBell` · `m_eEnteredWater` · `m_eBored`
- `m_eSendTo` · `m_eRunCrazy` · `m_eImmobilize`
- `m_eNPCDeath` · `m_eLeaveCombat` · `m_eRelax`
- `m_eSawDeadFriend` · `m_eAttack` · `m_eSawEnemyPlayer`
- `m_eLookAtSpot` · `m_ePlayerAmmoStrike` · `m_eNPCDamage`
- `m_eNPCGunshot` · `m_distortionTextureName` · `m_glareTextureName`
- `m_textureNames` · `m_textureFramerate` · `m_finalColor`
- `m_initialColor` · `m_cardAngularAccel` · `m_cardAngularVel`
- `m_cardInitAngle` · `m_finalScale` · `m_initialScale`
- `m_blendMode`

## DestructablePrefs  (37 fields)

- `m_aimRadius` · `m_onDestroyResetIntoPurgatory` · `m_updateNavOnChange`
- `m_animSpeedHi` · `m_animSpeedLo` · `m_animNumLoopsHi`
- `m_animNumLoopsLo` · `m_animPauseHi` · `m_animPauseLo`
- `m_animation` · `m_damageByNPCExplosion` · `m_damageByNPCShoot`
- `m_damageByNPCBeat` · `m_damageByExplosion` · `m_damageByShoot`
- `m_damageByBeat` · `m_damageByRam` · `m_particleVelocityMax`
- `m_particleVelocityMin` · `m_mass` · `m_centerZOffset`
- `m_deathBlowAlignEffectsToVelocity` · `m_rumbleStateChangeDuration` · `m_rumbleStateChange`
- `m_rumbleDestructDuration` · `m_rumbleDestruct` · `m_ambientCue`
- `m_cueDamage` · `m_cueDestruct` · `m_spawnCollectablesPrefsID`
- `m_effectDamagePrefsID` · `m_effectPrefsID` · `m_acceptDamageFromStranger`
- `m_killOnNPCContact` · `m_killOnPlayerContact` · `m_killOnAnyDamage`
- `m_maxHealth`

## MechanicalPrefs  (36 fields)

- `m_fireRateDeviation` · `m_fxWhileDamaged2` · `m_fxWhileDamaged1`
- `m_sndOnDeath` · `m_fxOnDeath` · `m_fxOnDamage`
- `m_idleDuringTransitions` · `m_sndIdle` · `m_sndTurretMove`
- `m_turretDeleteOnDeath` · `m_anims` · `m_turretRotationSpeedDegrees`
- `m_turretRotationLimitDegrees` · `m_turretMaxSwitchTime` · `m_turretMinSwitchTime`
- `m_turretMaxHoldOnTarget` · `m_turretRadius` · `m_turretCenterOffset`
- `m_turretHealth` · `m_modelName` · `m_registerModelDependency`
- `m_interactionResetTime` · `m_visibilityResetTime` · `m_exitIdleOnlyAtEnd`
- `m_animBlendTime` · `m_animReverseTime` · `m_idlesArePoses`
- `m_animsInterruptable` · `m_animsReversible` · `m_cyclical`
- `m_stateCount` · `m_damageStartsOn` · `m_activateTogglesActive`
- `m_activateForever` · `m_subTypePrefsID` · `m_mechSubType`

## ParticleSystem  (35 fields)

- `m_lockSpawnToCurrentPosition` · `m_particleDeathEffectName` · `m_particleEffectName`
- `m_suctionPoint` · `m_dontTransformAccel` · `m_screenAligned`
- `m_ignoreWindVelocity` · `m_useRelativeVelocity` · `m_colorPass`
- `m_maxSpawnPitch` · `m_minSpawnPitch` · `m_maxSpawnYaw`
- `m_minSpawnYaw` · `m_spawnAngle` · `m_spawnDirection`
- `m_spawnPoint2` · `m_spawnPoint1` · `m_midColor`
- `m_spawnColor` · `m_finalSize` · `m_spawnSize`
- `m_spawnAcceleration` · `m_particleAngularVelocity` · `m_maxSpawnVelocity`
- `m_minSpawnVelocity` · `m_maxParticleCount` · `m_initSpawnCount`
- `m_spawnRate` · `m_spawnDuration` · `m_particleLifetimeType`
- `m_suctionMode` · `m_durationType` · `m_spawnDirConstraint`
- `m_spawnPosConstraint` · `m_scrollSpeed`

## WorldTag  (34 fields)

- `m_exportInfo` · `m_fromTagfile` · `m_zoneAABB`
- `m_localLMDensity` · `m_overrideLMDensity` · `m_isInside`
- `m_zoneName` · `m_polygon` · `m_backZone`
- `m_frontZone` · `m_enabledByDefault` · `m_portalName`
- `m_polygonTo` · `m_polygonFrom` · `m_frameTo`
- `m_frameFrom` · `m_toZone` · `m_fromZone`
- `m_proxyZoneName` · `m_proxyZone` · `m_visibleFrom`
- `m_adjacentZone` · `m_zone` · `m_geometryToken`
- `m_predictedPlayerStartZone` · `m_worldIsSection` · `m_worldGeometries`
- `m_areaAdjacency` · `m_proxyVisibility` · `m_teleportPortals`
- `m_portals` · `m_zones` · `m_smbFileNames`
- `m_levelPrefs`

## RenderTarget  (33 fields)

- `m_sunBlobSpeed2` · `m_sunBlobSpeed1` · `m_sunSpikeIntensity`
- `m_sunSpikeColor2` · `m_sunSpikeColor1` · `m_sunBlobIntensity`
- `m_sunBlobColor2` · `m_sunBlobColor1` · `m_sunBlobSizeMax`
- `m_sunBlobSizeMin` · `m_sunSpikeSizeMax` · `m_sunSpikeSizeMin`
- `m_overrideFogColor` · `m_overrideFog` · `m_colorInvSunCloud`
- `m_colorSunCloud` · `m_colorBaseCloud` · `m_colorSun`
- `m_colorTop` · `m_colorBase` · `m_uvSpeed3`
- `m_uvSpeed2` · `m_uvSpeed1` · `m_texName3`
- `m_texName2` · `m_texName1` · `m_cloudTiling3`
- `m_cloudTiling2` · `m_cloudTiling1` · `m_cloudParallax2`
- `m_cloudParallax1` · `m_degreesNoFog` · `m_degreesFullFog`

## MotionImplDummy  (32 fields)

- `m_overVel_Run` · `m_overVel_Canter` · `m_overVel_Trot`
- `m_overVel_Walk` · `m_skidToStopCanterRunLerper` · `m_skidToStopTrotCanterLerper`
- `m_steefVelDriveSpeedDown` · `m_steefVelDriveSpeedUp` · `m_accelHi_Run`
- `m_accelLo_Run` · `m_accelHi_Canter` · `m_accelLo_Canter`
- `m_accelHi_Trot` · `m_accelLo_Trot` · `m_accelHi_Walk`
- `m_accelLo_Walk` · `m_jumpHeightSpeedScale` · `m_jumpHeightMax`
- `m_jumpHeightMin` · `m_maxRadiansToRotateLoSpeed` · `m_maxRadiansToRotateHiSpeed`
- `m_maxRadiansToRotateBasic` · `m_onlyStands` · `m_canBeKnocked`
- `m_canBePushed` · `m_gameSpeakTalkRestInterval` · `m_timeTillIdle2`
- `m_timeTillIdle1` · `m_waterVelocityScale` · `m_waterOffset`
- `m_bipedTurnSpeedDegrees_Panic` · `m_bipedTurnSpeedDegrees`

## EffectParticleSystem  (30 fields)

- `m_grabBonesFromObject` · `m_ignoreTargetBones` · `m_glarePass`
- `m_distortionPass` · `m_alphaPass` · `m_additivePass`
- `m_groundEffectName` · `m_airEffectName` · `m_additiveTextureName`
- `m_alphaTextureName` · `m_tesselateCount` · `m_tendrilGlareThickness`
- `m_tendrilDistortionThickness` · `m_tendrilAlphaThickness` · `m_tendrilAdditiveThickness`
- `m_tendrilDuration` · `m_curvature` · `m_textureScale`
- `m_maxRayCastAngle` · `m_minRayCastAngle` · `m_noCollideRadiusMax`
- `m_noCollideRadiusMin` · `m_outerRadius` · `m_innerRadius`
- `m_frequency3` · `m_frequency2` · `m_frequency1`
- `m_amplitude3` · `m_amplitude2` · `m_amplitude1`

## GloktigiPrefs  (28 fields)

- `m_gloktigiPhaseMinDistFromOtherGloktigi` · `m_gloktigiPhaseMinDistFromPlayer` · `m_gloktigiPhaseSpeed`
- `m_gloktigiPhaseMaxDist` · `m_gloktigiPhaseMinDist` · `m_gloktigiDamageAmountBleedDelay`
- `m_gloktigiDamageAmountBleedTime` · `m_gloktigiDamageAmountBeforePhase` · `m_gloktigiDamageDurationBeforePhase`
- `m_damageWindowMaxGap` · `m_glokPhaseEndFXUseAllFXBones` · `m_glokPhaseEndFXPref`
- `m_glokPhaseEndRmbPref` · `m_glokPhaseEndSndPref` · `m_glokPhaseDuringFXUseAllFXBones`
- `m_glokPhaseDuringFXPref` · `m_glokPhaseDuringRmbPref` · `m_glokPhaseDuringSndPref`
- `m_glokPhaseBeginFXUseAllFXBones` · `m_glokPhaseBeginFXPref` · `m_glokPhaseBeginRmbPref`
- `m_glokPhaseBeginSndPref` · `m_meleeDisableTime` · `m_otherGloktigiCheckDist`
- `m_gloktigiPhaseGeo` · `m_gloktigiGroup` · `m_gloktigiAlone`
- `m_geoToAttach`

## NPCWeaponPrefs  (27 fields)

- `m_hitFlashCount` · `m_hitFlashHeight` · `m_hitFlashWidth`
- `m_hitFlashTextureName` · `m_subBoltPrefs` · `m_subWeaponPrefs`
- `m_glokMeleeFireDelayFromSubWeapon` · `m_glokAttackFXPref` · `m_glokAttackRmbPref`
- `m_glokAttackSndPref` · `m_loopingFireEffectAttachBoneOverride` · `m_jumpBackTimeOverride`
- `m_leadWithAverageVelocity` · `m_actorTargetDetail` · `m_missTime`
- `m_accuracyWidth` · `m_countdown` · `m_spawnPref`
- `m_minDistance` · `m_hitConeHeight` · `m_hitConeAngleZ`
- `m_hitConeAngleXY` · `m_reloadTimeMax` · `m_reloadTime`
- `m_fireFromTextkeys` · `m_fireRate` · `m_weaponType`

## NPCShock  (26 fields)

- `m_dest` · `m_source` · `m_lifeTimeHi`
- `m_lifeTimeLo` · `m_spawnIntervalHi` · `m_spawnIntervalLo`
- `m_fx` · `m_playCueEachTime` · `m_cue`
- `m_fxdFirePopFX` · `m_fxdRecoverSmoke` · `m_fxdFiringArcs`
- `m_fxdFiringToTarget` · `m_fxdChargedFX` · `m_fxdChargingFX`
- `m_fxdChargeLadder` · `m_fxdIdleSpark` · `m_fxdIdleArc2`
- `m_fxdIdleArc` · `m_numArcsToTarget` · `m_duration_Recover`
- `m_duration_Firing` · `m_duration_Charging` · `m_extraKillTime`
- `m_cue_moving` · `m_cue_still`

## TextOverlayPrefs  (25 fields)

- `m_yOffset` · `m_xOffset` · `m_yAdvance`
- `m_xAdvance` · `m_texture` · `m_icons`
- `m_highlights` · `m_rectangles` · `m_verticalExpansionRate`
- `m_maxVerticalExpansion` · `m_minBottomInset` · `m_fadeRate`
- `m_maxLineWidth` · `m_doTwoStepScale` · `m_fontHeightPadding`
- `m_fontHeight` · `m_openDuration` · `m_scaleDuration`
- `m_edgeThickness` · `m_pressAtextColor` · `m_pressAedgeColor`
- `m_pressAbackgroundColor` · `m_textColor` · `m_edgeColor`
- `m_backgroundColor`

## CollectablePrefs  (23 fields)

- `m_typeID` · `m_artifactPrefsID` · `m_respawnDelay`
- `m_countMax` · `m_countMin` · `m_runDistOverride`
- `m_responsesUseUpperBody` · `m_runSpeedMultiplier` · `m_attachKnockTracksFacing`
- `m_attachKnockSpeed` · `m_attachIntervalRandomness` · `m_attachInterval`
- `m_attachDurationRandomness` · `m_attachDuration` · `m_attachDamage`
- `m_playHurtRandomness` · `m_playHurtInterval` · `m_attachEffect`
- `m_attachInitialCount` · `m_response` · `m_gibsActor`
- `m_damageType` · `m_exhaustRecoverTime`

## FlowMarkerPrefs  (22 fields)

- `m_flowMarkerTextureName` · `m_maxVelocityForMaxSpin` · `m_maxRotationalVelocity`
- `m_particleLifetime` · `m_currentCullDist` · `m_distortionOpaqueSpeed`
- `m_endDistortionFade` · `m_startDistortionFade` · `m_distortionMarkersCount`
- `m_debrisMarkersCount` · `m_maxDistortionSize` · `m_minDistortionSize`
- `m_maxDebrisSize` · `m_minDebrisSize` · `m_targetMarkerCount`
- `m_axis` · `m_midAlpha` · `m_startAlpha`
- `m_angularAccel` · `m_angularVel` · `m_initialAngle`
- `m_renderType`

## TownPanicPrefs  (20 fields)

- `m_emergeSteefCue` · `m_emergeOtherCue` · `m_insideSteefCue`
- `m_insideOtherCue` · `m_panicSteefCue` · `m_panicOtherCue`
- `m_alarmCue` · `m_bellRingRecoveryTimeNoTurrets` · `m_bellRingRecoveryTime`
- `m_turretFiredRecoveryTime` · `m_turretDeactivateRandomness` · `m_timeBetweenTurretDeactivates`
- `m_panicForever` · `m_followTeleportals` · `m_radiusMax`
- `m_beginningRadius` · `m_growRadiusBy` · `m_timeBetweenTurretActivates`
- `m_minDistToSteefSqr` · `m_mySpecies`

## NPCPrefsRare  (19 fields)

- `m_sndBoltHitTauntNoDamage` · `m_bossNameTextureName` · `m_useMotionSphereForBoltCollide`
- `m_isTouchyIfRammed` · `m_isTouchy` · `m_forceFieldPrefsName`
- `m_glokPrefsName` · `m_bountyableDuringImmobilize` · `m_bumpAnnoyanceTimeout`
- `m_armadilloStaminaMultiplier` · `m_skunkBombImmobilizeMultiplier` · `m_playerMeleeReturnExhaustRecoverTime`
- `m_playerMeleeReturnStamina` · `m_playerMeleeReturnDamageDestructable` · `m_playerMeleeReturnDamage`
- `m_playerMeleeReturnKnockback` · `m_playerMeleeKnocks` · `m_bombBatDamageMultiplier`
- `m_bombBatStaminaMultiplier`

## WaterPrefs  (19 fields)

- `m_distortionTex2` · `m_distortionTex1` · `m_distortionTex0`
- `m_distortionTexParams2` · `m_distortionTexParams1` · `m_distortionTexParams0`
- `m_mipmapBias` · `m_fresnelDistanceScale` · `m_maxFresnel`
- `m_minFresnel` · `m_specularStrength` · `m_renderSpecular`
- `m_reflectionColor` · `m_DBG_color` · `m_checkInView`
- `m_checkLOS` · `m_rangeBottom` · `m_rangeTop`
- `m_rangeXY`

## OrbitCamera  (18 fields)

- `m_breakFreeEffectPath` · `m_wrapEffectPath` · `m_cocoonMouthPath`
- `m_wrapPath` · `m_cocoonPath` · `m_maxRandomSpeechPitch`
- `m_minRandomSpeechPitch` · `m_rammedEffectName` · `m_sinkEffectName`
- `m_drownEffectName` · `m_smallSplashEffectName` · `m_splashEffectName`
- `m_splashVelocity` · `m_gibRumbleName` · `m_gibSoundName`
- `m_gibEffectName` · `m_species` · `m_facialPoseList`

## GameplayCameraGenerator  (18 fields)

- `m_paramLineEnd` · `m_paramLineStart` · `m_maxFieldChart`
- `m_zeroFieldChart` · `m_maxDist` · `m_minDist`
- `m_spline` · `m_originalTagFile` · `m_scriptResource`
- `m_scriptArgs` · `m_castLightmapShadows` · `m_startsInPurgatory`
- `m_groupName` · `m_snapToGround` · `m_illuminationColorDW`
- `m_tagTransform` · `m_isInProxyZone` · `m_tagZone`

## DummyTrackGroup  (18 fields)

- `m_dir` · `m_started` · `m_tanHalfFov`
- `m_worldToView` · `m_viewToWorld` · `m_fovRad`
- `m_zFar` · `m_zNear` · `m_z`
- `m_y` · `m_x` · `m_radii`
- `m_center` · `m_axes` · `m_max`
- `m_min` · `m_bbox` · `m_pieces`

## CoverDuration  (17 fields)

- `m_hideVolSeeDistance` · `m_instantSightDistance` · `m_verticalAngleDeg`
- `m_horizontalAngleDeg` · `m_seeBelow` · `m_seeAbove`
- `m_seeDistance` · `m_6thSenseDistance` · `m_attackParams`
- `m_relaxAgitatedToNormal` · `m_maxWaitForConversation` · `m_minWaitForConversation`
- `m_allowPanic` · `m_sightPanic` · `m_sightCombat`
- `m_sightAgit` · `m_sightNormal`

## PhysParticleSystem  (17 fields)

- `m_impactSndWater` · `m_impactSnd` · `m_density`
- `m_avgRotationalSpeedFactor` · `m_avgRotationalSpeed` · `m_avgVelocityFactor`
- `m_avgVelocity` · `m_avgSpeedFactor` · `m_avgSpeed`
- `m_particleCountFactor` · `m_particleCount` · `m_startColor`
- `m_dimensionVelocity` · `m_dimension` · `m_fadeOutPercent`
- `m_fadeInPercent` · `m_position`

## ForceFieldInst  (17 fields)

- `m_forceFieldGlowColor` · `m_impactRippleSizeScale` · `m_impactRippleAgeMax`
- `m_numImpactRipplesMax` · `m_forceFieldScrollVelocities` · `m_forceFieldDistortionFresnelScale`
- `m_forceFieldEndFade` · `m_forceFieldStartFade` · `m_flickerThreshold`
- `m_flickerSineOffset2` · `m_flickerSineFrequencyScale2` · `m_flickerSineOffset1`
- `m_flickerSineFrequencyScale1` · `m_flickerSineOffset0` · `m_flickerSineFrequencyScale0`
- `m_doOnOffFlicker` · `m_onOffFadeDuration`

## ExplosionPrefs  (16 fields)

- `m_collide` · `m_knockFollowcamUsesFootZ` · `m_eventRangeBottom`
- `m_eventRangeTop` · `m_eventRangeXY` · `m_cueExplode`
- `m_rumbleDuration` · `m_rumble` · `m_effect`
- `m_knockKnocksDown` · `m_knockSpeedPlayer` · `m_knockSpeed`
- `m_staminaPlayer` · `m_damagePlayer` · `m_damageAtAreaEdgeMultiplier`
- `m_explodeDuration`

## GeometryInst  (16 fields)

- `m_baseShadowOffset` · `m_useOldShadowTechnique` · `m_edgeBurnColor`
- `m_edgeBurnThickness` · `m_transparentZWriteAlphaRef` · `m_phasedScrollVelocityY`
- `m_phasedScrollVelocityX` · `m_phasedGlowScale` · `m_phasedEndFade`
- `m_phasedStartFade` · `m_detailMappedEndFade` · `m_detailMappedStartFade`
- `m_normalMappedEndFade` · `m_normalMappedStartFade` · `m_reflectionMappedEndFade`
- `m_reflectionMappedStartFade`

## SparkParticleSystem  (14 fields)

- `m_collideMod` · `m_sparkTailTextureName` · `m_sparkTipEndColor`
- `m_sparkTipStartColor` · `m_sparkTailEndColor` · `m_sparkTailStartColor`
- `m_sparkGlareLength` · `m_sparkLength` · `m_sparkGlareThickness`
- `m_sparkTailThickness` · `m_sparkLeadThickness` · `m_sparkTipRadius`
- `m_sparkBounceFactor` · `m_sparkThickness`

## CollectableSpawner  (13 fields)

- `m_items` · `m_filterPanic` · `m_filterCombat`
- `m_filterAgit` · `m_filterNormal` · `m_maxOccupants`
- `m_startActive` · `m_filter` · `m_priority`
- `m_guardRadius` · `m_radius` · `m_fieldInRadians`
- `m_facing`

## RumblePrefs  (13 fields)

- `m_screenRandomRotBleedbackTime` · `m_screenRandomRotOffset` · `m_screenRandomRotRange`
- `m_screenRandomUseGaussian` · `m_screenVertical` · `m_screenHorizontal`
- `m_controllerRight` · `m_controllerLeft` · `m_shakePlayerWeapon`
- `m_persistent` · `m_baseFOVScaleDegrees` · `m_screenSpatialization`
- `m_controllerSpatialization`

## DreadlockSet  (13 fields)

- `m_renderLate` · `m_visibilityRadius` · `m_waterSurface`
- `m_worldOriented` · `m_glareEndFadeRadius` · `m_glareStartFadeRadius`
- `m_endFadeRadius` · `m_startFadeRadius` · `m_debugLoop`
- `m_rotationAxis` · `m_effectLifetime` · `m_centerOfRotation`
- `m_angularVelocity`

## DamagePrefs  (12 fields)

- `m_sndResponseBase` · `m_affectsDeadGuys` · `m_forceOverridePerVolumeSettings`
- `m_deathSpeech` · `m_damageSpeech` · `m_damageCue`
- `m_knocksOffPipes` · `m_onlyInVolume` · `m_useNPCSettingForPlayer`
- `m_player` · `m_npc` · `m_animations`

## JobTag  (12 fields)

- `m_closestNavPointIndex` · `m_closestNavPointZone` · `m_globalVarValue`
- `m_dependOnGlobalVar` · `m_restrictToBrainState` · `m_restrictToGuyType`
- `m_restrictToLeader` · `m_restrictToSpecificGuy` · `m_relax`
- `m_checkEverySeconds` · `m_minor` · `m_major`

## SpotLightTag  (12 fields)

- `m_indoor` · `m_fill` · `m_affectsLM`
- `m_affectsRT` · `m_shadows` · `m_normalHardness`
- `m_maxIntensity` · `m_intensity` · `m_color`
- `m_outerAngle` · `m_innerAngle` · `m_direction`

## TumbleParticleSystem  (12 fields)

- `m_offsetScale` · `m_turbulenceCoeff3` · `m_turbulenceCoeff2`
- `m_turbulenceCoeff1` · `m_turbulenceCoeff0` · `m_turbulenceFreqVelocity`
- `m_turbulenceCoeffVelocity` · `m_falloffDistance` · `m_particleSize`
- `m_particleDensity` · `m_diffuseColor` · `m_constantVelocity`

## CritterPathPrefs  (11 fields)

- `m_spawnEnabled` · `m_stopRandomness` · `m_stopFrequency`
- `m_spawnRandomness` · `m_spawnFrequency` · `m_fleeSpeed`
- `m_travelSpeed` · `m_npcFleeDistance` · `m_playerFleeDistance`
- `m_spawnCapacity` · `m_count`

## TownPanicController  (11 fields)

- `m_maxSpin` · `m_minSpin` · `m_maxSize`
- `m_minSize` · `m_randomRadius` · `m_numParticles`
- `m_maxSpeedRate` · `m_maxSpeed` · `m_minSpeedRate`
- `m_minSpeed` · `m_fractionSpawned`

## NPCTag  (11 fields)

- `m_feetFixed` · `m_radarLocationInPurgatory` · `m_script`
- `m_maxDistanceFromHome` · `m_whenNotSeen` · `m_randomFacing`
- `m_spawnScatterRadius` · `m_minSpawnInterval` · `m_maxAliveAtATime`
- `m_countToSpawn` · `m_npcPref`

## GameTagBase  (10 fields)

- `m_hSpeedRandomness` · `m_hSpeed` · `m_vSpeedRandomness`
- `m_vSpeed` · `m_inheritSpawnersVelocity` · `m_tumble`
- `m_dynamic` · `m_spawnRadius` · `m_spawnCount`
- `m_prefsID`

## CameraVolumeTag  (9 fields)

- `m_inOthers` · `m_inBoat` · `m_inPipe`
- `m_allowPOVCam` · `m_transOutDefaultEase` · `m_transOutDefault`
- `m_transInEase` · `m_transIn` · `m_cameraName`

## DiskEffectInst  (9 fields)

- `m_bumpTextureName` · `m_finalAlpha` · `m_initAlpha`
- `m_initHeight` · `m_initHeightVel` · `m_heightAccel`
- `m_initRadius` · `m_initRadiusVel` · `m_radiusAccel`

## TagIlluminationTag  (8 fields)

- `m_illuminationColor` · `m_height` · `m_depth`
- `m_width` · `m_metaData` · `m_flowRate`
- `m_rightSpline` · `m_leftSpline`

## CritterCuePrefs  (8 fields)

- `m_playerLandCue` · `m_playerJumpCue` · `m_knockCue`
- `m_idleColorCue` · `m_reloadedCue` · `m_selectedFromHUDCue`
- `m_congratulatoryCue` · `m_takeDamageCue`

## Wolvark_06_5_cb_Tag_Decorator_sekto_charge_lights1  (7 fields)

- `m_laserBeamPrefsName` · `m_chargeObjectName` · `m_ringRotationRateDegrees`
- `m_bigShotKnockVel` · `m_bigShotRayRadius` · `m_bigShotDamageRate`
- `m_duration_BigShot`

## PipeTag  (7 fields)

- `m_useFacing` · `m_npcPrefsToken` · `m_drawSightCone`
- `m_blinkFreq` · `m_blinking` · `m_active`
- `m_blipColor`

## CardEffectInst  (7 fields)

- `m_yDirection` · `m_xDirection` · `m_textureName`
- `m_finalInnerRadius` · `m_finalOuterRadius` · `m_initialInnerRadius`
- `m_initialOuterRadius`

## Animations  (6 fields)

- `m_blendTime` · `m_randomSpeed` · `m_randomOffset`
- `m_autoStart` · `m_motionType` · `m_name`

## SnapIn  (6 fields)

- `m_states` · `m_weaponLoadedTransition` · `m_weaponEmptyTransition`
- `m_triggerUpTransition` · `m_triggerDownTransition` · `m_timeoutTransition`

## ArtifactPrefs  (6 fields)

- `m_upgradeOrder` · `m_parameter` · `m_3DModelScale`
- `m_3DModelName` · `m_sellValue` · `m_buyValue`

## SkyPrefs  (6 fields)

- `m_skyParams` · `m_flares` · `m_flaresTweakSize`
- `m_flaresAdditive` · `m_skyDownFrac` · `m_skyStretch`

## CameraContext  (5 fields)

- `m_yawVel` · `m_pitchVel` · `m_yaw`
- `m_pitch` · `m_allowInput`

## PlayerArmorPrefs  (5 fields)

- `m_knuckleAttachment` · `m_ram` · `m_punch`
- `m_headButt` · `m_buckAttack`

## ExplosivePrefs  (5 fields)

- `m_aimCenterZOffset` · `m_destroyByNPCExplosion` · `m_destroyByNPCShoot`
- `m_destroyByPlayerShoot` · `m_boltEjectSpeed`

## CinematicCameraTag  (5 fields)

- `m_timeEnd` · `m_timeStart` · `m_animationToken`
- `m_followObject` · `m_fov`

## CritterPathTag  (5 fields)

- `m_npcRangeOverride` · `m_playerRangeOverride` · `m_spawnPrefOverride`
- `m_prefPath` · `m_controls`

## AudioLevelPrefs  (5 fields)

- `m_lipBundleGlobalIndex` · `m_lipBundleStem` · `m_musicCueName`
- `m_regionWaveBankStem` · `m_regionDebugName`

## DecoratorPrefs  (5 fields)

- `m_fadeDistance` · `m_forceFullRotation` · `m_fadeOutInDistance`
- `m_alphaRef` · `m_alphaBlend`

## VMInstanceInternal  (5 fields)

- `m_speedOverride` · `m_leftAnimPath` · `m_rightAnimPath`
- `m_negativeAnimPath` · `m_positiveAnimPath`

## ForceFieldPrefs  (4 fields)

- `m_xform` · `m_forceFieldInstPrefsName` · `m_boneToAttachTo`
- `m_forceFieldGeo`

## StorePrefs  (4 fields)

- `m_itemDefaultsOther` · `m_itemDefaultsUpgrades` · `m_itemDefaultsArmor`
- `m_itemDefaultsAmmo`

## DamageVolumeTag  (4 fields)

- `m_startsActive` · `m_prefs` · `m_damageRateNPC`
- `m_damageRatePlayer`

## DecoratorIlluminationTag  (4 fields)

- `m_illuminationList` · `m_vertexColorStreamName` · `m_pathToken`
- `m_tagName`

## RadarPrefs  (4 fields)

- `m_writeWarningTextureName` · `m_writeWarningSizeScale` · `m_writeWarningPosY`
- `m_writeWarningPosX`

## RenderContext  (4 fields)

- `m_a` · `m_b` · `m_g`
- `m_r`

## AnimationControl  (4 fields)

- `m_yHi` · `m_yLo` · `m_xHi`
- `m_xLo`

## StaticObject  (3 fields)

- `m_quantity` · `m_priceOverride` · `m_id`

## Basic  (3 fields)

- `m_perc` · `m_text` · `m_background`

## AnimationLayerConfig  (3 fields)

- `m_sources` · `m_which` · `m_version`

## AlarmTag  (3 fields)

- `m_soundToMake` · `m_timeBetweenSounds` · `m_howLongToSound`

## AudioVolumeTag  (3 fields)

- `m_linkedTo` · `m_occluderIndex` · `m_environmentIndex`

## InstancedObjectTag  (3 fields)

- `m_lmInfo` · `m_lightmapID` · `m_modelToken`

## LightmapperLMInfoTag  (3 fields)

- `m_infos` · `m_packingInfo` · `m_packedResource`

## SimpleActivatableInstanceTag  (3 fields)

- `m_desiredState` · `m_triggerType` · `m_activateTarget`

## EffectMix  (3 fields)

- `m_effectName` · `m_timeOffset` · `m_effects`

## BountyAssignmentPrefs  (3 fields)

- `m_targetObjectNameStringID` · `m_targetObjectName` · `m_bountyDescription`

## ForceFieldInstPrefs  (3 fields)

- `m_t` · `m_mat` · `m_knots`

## DefaultArmor  (2 fields)

- `m_motion` · `m_geo`

## BountyPostPrefs  (2 fields)

- `m_uiSWF` · `m_rewardFactor`

## DecoratorTag  (2 fields)

- `m_decoratorResourceToken` · `m_prefsName`

## EffectLocation  (2 fields)

- `m_preferenceFile` · `m_spawnAtStartup`

## FixedCameraGenerator  (2 fields)

- `m_fieldChart` · `m_allowLook`

## RegroupTag  (2 fields)

- `m_occupancyLimit` · `m_regroupSpotFor`

## ScriptVolumeTag  (2 fields)

- `m_playerSpeechCue` · `m_execCount`

## Level_HalfVectorCubemap  (2 fields)

- `m_angle` · `m_velocity`

## GeoParticleSystem  (2 fields)

- `m_startFadePercent` · `m_useVelocityAxis`

## LevelListPrefs  (2 fields)

- `m_displayNames` · `m_paths`


## Enum constants (modes / buttons / channels / states)

- `eAction` · `eAgit` · `eAmmoBag` · `eAny` · `eArmor`
- `eAttack` · `eBack` · `eBasic` · `eBigShot` · `eBored`
- `eBountySpeedMultiplier` · `eBreak` · `eBreederBag` · `eBuckAttack` · `eBurstToRun`
- `eCamLookAround` · `eCamLookAroundHold` · `eCantInit` · `eChangeDisguise` · `eChannelAmbient`
- `eChannelAmbientLooping` · `eChannelAmbientRiver` · `eChannelByCue` · `eChannelCombat` · `eChannelIntermittent`
- `eChannelMisc` · `eChannelMotion` · `eChannelNull` · `eChannelPlayerCombat` · `eChannelPlayerMotion`
- `eChannelPlayerSpeech` · `eChannelPriority` · `eChannelProjectile` · `eChannelSpeech` · `eChannelTest`
- `eChannelUI` · `eCharged` · `eCharging` · `eCheatModifier` · `eClipReload`
- `eCombat` · `eCombatWindDown` · `eCritterAttracter` · `eCrouch` · `eCycleCheatCamDamping`
- `eDebugCycleUpgradeDown` · `eDebugCycleUpgradeUp` · `eDebugDeleteAllSaveGames` · `eDebugGenericAction` · `eDebugGiveAllAmmo`
- `eDebugInvincible` · `eDebugPause` · `eDebugPick` · `eDebugReload` · `eDone`
- `eEast` · `eEnduranceMax` · `eEnduranceRegen` · `eEnteredWater` · `eExplosion`
- `eFinalMutter` · `eFinished` · `eFireLeft` · `eFireLeftCharge` · `eFireRight`
- `eFireRightCharge` · `eFiring` · `eFlyCam` · `eFollow` · `eForceJob`
- `eFreezeCam` · `eFront` · `eGameplay` · `eGoingHome` · `eGotoPoint`
- `eHiding` · `eHoldSlowMotion` · `eHowl` · `eIdle` · `eImmobilize`
- `eImportant` · `eInit` · `eInsulted` · `eInsulter` · `eIntersecting`
- `eInventoryActivate` · `eJump` · `eKeepPolling` · `eLayDown` · `eLeaveCombat`
- `eLeaving` · `eLockFollowCam` · `eLookAtSpot` · `eNPCDamage` · `eNPCDeath`
- `eNPCGunshot` · `eNPCShout` · `eNormal` · `eNorth` · `eNorthEast`
- `eNorthWest` · `eOnCompanyProperty` · `eOther` · `ePanic` · `ePeckShot`
- `ePlayerAmmoStrike` · `ePlayerNoise` · `ePress` · `ePunchManual` · `eReaction`
- `eRecenterCamera` · `eRecover` · `eRelax` · `eReset` · `eResetPos`
- `eRingBell` · `eRunCrazy` · `eRunToDoor` · `eRunToRegroup` · `eSawDeadFriend`
- `eSawDeadPlayer` · `eSawEnemyPlayer` · `eSawSteefChange` · `eSendTo` · `eShakeOffDamage`
- `eShakeOffDamageLoadoutScheme` · `eSkipCinematic` · `eSouth` · `eSouthEast` · `eSouthWest`
- `eSpeaking` · `eStop` · `eStopTurnAround` · `eStopTurnTowards` · `eStruggle`
- `eTalkRequest` · `eTalkToSteef` · `eTalkToStranger` · `eToggleFirstPerson` · `eToggleGrab`
- `eToggleSniperView` · `eTownBell` · `eTryNow` · `eUIBack` · `eUICycleActiveLoadoutSlotAlt`
- `eUICycleActiveLoadoutSlotPrimary` · `eUICycleWeapSelectAlt` · `eUICycleWeapSelectPrimary` · `eUIDown` · `eUILeft`
- `eUIPause` · `eUIRight` · `eUISelect` · `eUIToggleWeaponLoadoutScreen` · `eUIUp`
- `eUse` · `eVisual` · `eWFDLeaving` · `eWait` · `eWaitForDoor`
- `eWaitForEnd` · `eWaitForPanicSpot` · `eWaitForResponse` · `eWaitingForChanceToSpeak` · `eWeaponCategoryActivate`
- `eWeaponCategoryDamage` · `eWeaponCategoryImmobilize` · `eWeaponCategorySendTo` · `eWeaponCategoryTrap` · `eWeaponCursorDown`
- `eWeaponCursorUp` · `eWeaponCycleSelectedAlt` · `eWeaponCycleSelectedAltFromLoadout` · `eWeaponCycleSelectedModifier` · `eWeaponCycleSelectedPrimary`
- `eWeaponCycleSelectedPrimaryFromLoadout` · `eWeaponSelectLeft` · `eWeaponSelectRight` · `eWeaponSelectToFireLeft` · `eWeaponSelectToFireRight`
- `eWest`