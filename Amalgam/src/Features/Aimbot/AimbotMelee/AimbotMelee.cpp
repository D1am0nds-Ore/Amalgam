#include "AimbotMelee.h"

#include "../../Simulation/MovementSimulation/MovementSimulation.h"
#include "../../TickHandler/TickHandler.h"
#include "../../Visuals/Visuals.h"

std::vector<Target_t> CAimbotMelee::GetTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	std::vector<Target_t> validTargets;

	const Vec3 vLocalPos = pLocal->GetShootPos();
	const Vec3 vLocalAngles = I::EngineClient->GetViewAngles();

	if (Vars::Aimbot::General::Target.Value & PLAYER)
	{
		const bool bDisciplinary = Vars::Aimbot::Melee::WhipTeam.Value && pWeapon->m_iItemDefinitionIndex() == Soldier_t_TheDisciplinaryAction;
		for (auto pEntity : H::Entities.GetGroup(bDisciplinary ? EGroupType::PLAYERS_ALL : EGroupType::PLAYERS_ENEMIES))
		{
			auto pPlayer = pEntity->As<CTFPlayer>();
			if (pPlayer == pLocal || !pPlayer->IsAlive() || pPlayer->IsAGhost())
				continue;

			if (F::AimbotGlobal.ShouldIgnore(pPlayer, pLocal, pWeapon))
				continue;

			Vec3 vPos = pPlayer->GetCenter();
			Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPos);
			const float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);

			if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
				continue;

			const float flDistTo = vLocalPos.DistTo(vPos);
			const int priority = F::AimbotGlobal.GetPriority(pPlayer->entindex());
			validTargets.push_back({ pPlayer, ETargetType::PLAYER, vPos, vAngleTo, flFOVTo, flDistTo, priority });
		}
	}

	if (Vars::Aimbot::General::Target.Value)
	{
		bool bHasWrench = pWeapon->GetWeaponID() == TF_WEAPON_WRENCH, bCanDestroySapper = false;
		switch (pWeapon->m_iItemDefinitionIndex())
		{
		case Pyro_t_Homewrecker:
		case Pyro_t_TheMaul:
		case Pyro_t_NeonAnnihilator:
		case Pyro_t_NeonAnnihilatorG:
			bCanDestroySapper = true;
		}

		for (auto pEntity : H::Entities.GetGroup(bHasWrench || bCanDestroySapper ? EGroupType::BUILDINGS_ALL : EGroupType::BUILDINGS_ENEMIES))
		{
			auto pBuilding = pEntity->As<CBaseObject>();

			bool bSentry = pBuilding->IsSentrygun(), bDispenser = pBuilding->IsDispenser(), bTeleporter = pBuilding->IsTeleporter();
			if (bSentry && !(Vars::Aimbot::General::Target.Value & SENTRY)
				|| bDispenser && !(Vars::Aimbot::General::Target.Value & DISPENSER)
				|| bTeleporter && !(Vars::Aimbot::General::Target.Value & TELEPORTER))
				continue;

			if (pBuilding->m_iTeamNum() == pLocal->m_iTeamNum() && (bHasWrench && !AimFriendlyBuilding(pBuilding) || bCanDestroySapper && !pBuilding->m_bHasSapper()))
				continue;

			Vec3 vPos = pBuilding->GetCenter();
			Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPos);
			const float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);
			const float flDistTo = vLocalPos.DistTo(vPos);

			if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
				continue;

			validTargets.push_back({ pBuilding, bSentry ? ETargetType::SENTRY : bDispenser ? ETargetType::DISPENSER : ETargetType::TELEPORTER, vPos, vAngleTo, flFOVTo, flDistTo });
		}
	}

	if (Vars::Aimbot::General::Target.Value & NPC)
	{
		for (auto pNPC : H::Entities.GetGroup(EGroupType::WORLD_NPC))
		{
			Vec3 vPos = pNPC->GetCenter();
			Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPos);

			const float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);
			const float flDistTo = vLocalPos.DistTo(vPos);

			if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
				continue;

			validTargets.push_back({ pNPC, ETargetType::NPC, vPos, vAngleTo, flFOVTo, flDistTo });
		}
	}

	return validTargets;
}

bool CAimbotMelee::AimFriendlyBuilding(CBaseObject* pBuilding)
{
	if (!pBuilding->m_bMiniBuilding() && pBuilding->m_iUpgradeLevel() != 3 || pBuilding->m_iHealth() < pBuilding->m_iMaxHealth() || pBuilding->m_bHasSapper())
		return true;

	if (pBuilding->IsSentrygun())
	{
		int iShells, iMaxShells, iRockets, iMaxRockets; pBuilding->As<CObjectSentrygun>()->GetAmmoCount(iShells, iMaxShells, iRockets, iMaxRockets);
		if (iShells < iMaxShells || iRockets < iMaxRockets)
			return true;
	}

	return false;
}

std::vector<Target_t> CAimbotMelee::SortTargets(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	auto validTargets = GetTargets(pLocal, pWeapon);

	const auto& sortMethod = ESortMethod::DISTANCE; //static_cast<ESortMethod>(Vars::Aimbot::Melee::SortMethod.Value);
	F::AimbotGlobal.SortTargets(&validTargets, sortMethod);

	std::vector<Target_t> sortedTargets = {};
	int i = 0; for (auto& target : validTargets)
	{
		i++; if (i > Vars::Aimbot::General::MaxTargets.Value) break;

		sortedTargets.push_back(target);
	}

	F::AimbotGlobal.SortPriority(&sortedTargets);

	return sortedTargets;
}



int CAimbotMelee::GetSwingTime(CTFWeaponBase* pWeapon)
{
	if (pWeapon->GetWeaponID() == TF_WEAPON_KNIFE)
		return 0;
	return Vars::Aimbot::Melee::SwingTicks.Value;
}

void CAimbotMelee::SimulatePlayers(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, std::vector<Target_t> targets,
								   Vec3& vEyePos, std::unordered_map<CBaseEntity*, std::deque<TickRecord>>& pRecordMap,
								   std::unordered_map<CBaseEntity*, std::deque<Vec3>>& simLines)
{
	// swing prediction / auto warp
	const int iSwingTicks = GetSwingTime(pWeapon);
	int iMax = (iDoubletapTicks && Vars::CL_Move::Doubletap::AntiWarp.Value && pLocal->m_hGroundEntity())
		? std::max(iSwingTicks - Vars::CL_Move::Doubletap::TickLimit.Value - 1, 0)
		: std::max(iSwingTicks, iDoubletapTicks);

	if ((Vars::Aimbot::Melee::SwingPrediction.Value || iDoubletapTicks) && pWeapon->m_flSmackTime() < 0.f && iMax)
	{
		PlayerStorage localStorage;
		std::unordered_map<CBaseEntity*, PlayerStorage> targetStorage;

		F::MoveSim.Initialize(pLocal, localStorage, false, iDoubletapTicks);
		for (auto& target : targets)
			F::MoveSim.Initialize(target.m_pEntity, targetStorage[target.m_pEntity], false);

		for (int i = 0; i < iMax; i++) // intended for plocal to collide with targets
		{
			if (i < iMax)
			{
				if (pLocal->IsCharging() && iMax - i <= GetSwingTime(pWeapon)) // demo charge fix for swing pred
				{
					localStorage.m_MoveData.m_flMaxSpeed = pLocal->TeamFortress_CalculateMaxSpeed(true);
					localStorage.m_MoveData.m_flClientMaxSpeed = localStorage.m_MoveData.m_flMaxSpeed;
				}
				F::MoveSim.RunTick(localStorage);
			}
			if (i < iSwingTicks - iDoubletapTicks)
			{
				for (auto& target : targets)
				{
					F::MoveSim.RunTick(targetStorage[target.m_pEntity]);
					if (!targetStorage[target.m_pEntity].m_bFailed)
						pRecordMap[target.m_pEntity].push_front({
							targetStorage[target.m_pEntity].m_bPredictNetworked ? target.m_pEntity->m_flSimulationTime() + TICKS_TO_TIME(i + 1) : 0.f,
							{},
							targetStorage[target.m_pEntity].m_vPredictedOrigin
						});
				}
			}
		}
		vEyePos = localStorage.m_MoveData.m_vecAbsOrigin + pLocal->m_vecViewOffset();

		if (Vars::Visuals::Simulation::SwingLines.Value)
		{
			const bool bAlwaysDraw = !Vars::Aimbot::General::AutoShoot.Value || Vars::Debug::Info.Value;
			if (!bAlwaysDraw)
			{
				simLines[pLocal] = localStorage.PredictionLines;
				for (auto& target : targets)
					simLines[target.m_pEntity] = targetStorage[target.m_pEntity].PredictionLines;
			}
			else
			{
				G::LinesStorage.clear();
				G::LinesStorage.push_back({ localStorage.PredictionLines, I::GlobalVars->curtime + 5.f, Vars::Colors::ProjectileColor.Value });
				for (auto& target : targets)
					G::LinesStorage.push_back({ targetStorage[target.m_pEntity].PredictionLines, I::GlobalVars->curtime + 5.f, Vars::Colors::PredictionColor.Value });
			}
		}

		F::MoveSim.Restore(localStorage);
		for (auto& target : targets)
			F::MoveSim.Restore(targetStorage[target.m_pEntity]);
	}
}

bool CAimbotMelee::CanBackstab(CBaseEntity* pTarget, CTFPlayer* pLocal, Vec3 vEyeAngles)
{
	if (!pLocal || !pTarget)
		return false;

	if (Vars::Aimbot::Melee::IgnoreRazorback.Value)
	{
		CUtlVector<CBaseEntity*> itemList;
		int iBackstabShield = SDK::AttribHookValue(0, "set_blockbackstab_once", pTarget, &itemList);
		if (iBackstabShield && itemList.Count())
		{
			CBaseEntity* pEntity = itemList.Element(0);
			if (pEntity && pEntity->ShouldDraw())
				return false;
		}
	}

	Vec3 vToTarget = pTarget->GetAbsOrigin() - pLocal->m_vecOrigin();
	vToTarget.z = 0.f;
	const float flDist = vToTarget.Length();
	if (!flDist)
		return false;

	vToTarget.Normalize();
	float flTolerance = 0.0625f;
	float flExtra = 2.f * flTolerance / flDist; // account for origin tolerance

	float flPosVsTargetViewMinDot = 0.f + 0.0031f + flExtra;
	float flPosVsOwnerViewMinDot = 0.5f + flExtra;
	float flViewAnglesMinDot = -0.3f + 0.0031f; // 0.00306795676297 ?

	auto TestDots = [&](Vec3 vTargetAngles)
		{
			Vec3 vOwnerForward; Math::AngleVectors(vEyeAngles, &vOwnerForward);
			vOwnerForward.z = 0.f;
			vOwnerForward.Normalize();

			Vec3 vTargetForward; Math::AngleVectors(vTargetAngles, &vTargetForward);
			vTargetForward.z = 0.f;
			vTargetForward.Normalize();

			const float flPosVsTargetViewDot = vToTarget.Dot(vTargetForward); // Behind?
			const float flPosVsOwnerViewDot = vToTarget.Dot(vOwnerForward); // Facing?
			const float flViewAnglesDot = vTargetForward.Dot(vOwnerForward); // Facestab?

			return flPosVsTargetViewDot > flPosVsTargetViewMinDot && flPosVsOwnerViewDot > flPosVsOwnerViewMinDot && flViewAnglesDot > flViewAnglesMinDot;
		};

	Vec3 vTargetAngles = { 0.f, H::Entities.GetEyeAngles(pTarget).y, 0.f };
	if (!Vars::Aimbot::Melee::BackstabAccountPing.Value)
	{
		if (!TestDots(vTargetAngles))
			return false;
	}
	else
	{
		if (Vars::Aimbot::Melee::BackstabDoubleTest.Value && !TestDots(vTargetAngles))
			return false;

		vTargetAngles.y += H::Entities.GetPingAngles(pTarget).y;
		if (!TestDots(vTargetAngles))
			return false;
	}

	return true;
}

int CAimbotMelee::CanHit(Target_t& target, CTFPlayer* pLocal, CTFWeaponBase* pWeapon, Vec3 vEyePos, std::deque<TickRecord> newRecords)
{
	if (Vars::Aimbot::General::Ignore.Value & UNSIMULATED && H::Entities.GetChoke(target.m_pEntity) > Vars::Aimbot::General::TickTolerance.Value)
		return false;

	float flHull = SDK::AttribHookValue(18, "melee_bounds_multiplier", pWeapon);
	float flRange = pWeapon->GetSwingRange(pLocal);
	if (pLocal->m_flModelScale() > 1.0f)
	{
		flRange *= pLocal->m_flModelScale();
		flRange *= pLocal->m_flModelScale();
		flRange *= pLocal->m_flModelScale();
	}
	flRange = SDK::AttribHookValue(flRange, "melee_range_multiplier", pWeapon);
	if (flHull <= 0.f || flRange <= 0.f)
		return false;

	static Vec3 vSwingMins = { -flHull, -flHull, -flHull };
	static Vec3 vSwingMaxs = { flHull, flHull, flHull };

	CGameTrace trace = {};
	CTraceFilterHitscan filter = {}; filter.pSkip = pLocal;

	std::deque<TickRecord> vRecords;
	{
		auto pRecords = F::Backtrack.GetRecords(target.m_pEntity);
		if (pRecords && target.m_TargetType == ETargetType::PLAYER)
		{
			if (Vars::Backtrack::Enabled.Value)
				vRecords = *pRecords;
			else
			{
				vRecords = F::Backtrack.GetValidRecords(pRecords, pLocal);
				if (!vRecords.empty())
					vRecords = { vRecords.front() };
			}
		}
		if (!pRecords || vRecords.empty())
		{
			matrix3x4 aBones[MAXSTUDIOBONES];
			if (!target.m_pEntity->SetupBones(aBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, target.m_pEntity->m_flSimulationTime()))
				return false;

			vRecords.push_front({
				target.m_pEntity->m_flSimulationTime(),
				*reinterpret_cast<BoneMatrix*>(&aBones),
				target.m_pEntity->m_vecOrigin()
			});
		}
	}
	if (!newRecords.empty())
	{
		for (TickRecord& pTick : newRecords)
		{
			vRecords.pop_back();
			vRecords.push_front({ pTick.flSimTime, {}, pTick.vOrigin });
		}
		for (TickRecord& pTick : vRecords)
			pTick.flSimTime -= TICKS_TO_TIME(newRecords.size());
	}
	std::deque<TickRecord> validRecords = target.m_TargetType == ETargetType::PLAYER ? F::Backtrack.GetValidRecords(&vRecords, pLocal, true) : vRecords;
	if (!Vars::Backtrack::Enabled.Value && !validRecords.empty())
		validRecords = { validRecords.front() };

	// this might be retarded
	const float flTargetPos = (target.m_pEntity->m_vecMaxs().z - target.m_pEntity->m_vecMins().z) * 65.f / 82.f;
	const float flLocalPos = (pLocal->m_vecMaxs().z - pLocal->m_vecMins().z) * 65.f / 82.f;
	const Vec3 vDiff = { 0, 0, std::min(flTargetPos, flLocalPos) };

	auto originTolerance = [target](float flTolerance)
		{
			target.m_pEntity->m_vecMins() += flTolerance;
			target.m_pEntity->m_vecMaxs() -= flTolerance;
		};

	// account for origin tolerance
	originTolerance(0.125f);

	for (auto& pTick : validRecords)
	{
		const Vec3 vRestore = target.m_pEntity->GetAbsOrigin();
		target.m_pEntity->SetAbsOrigin(pTick.vOrigin);

		target.m_vPos = pTick.vOrigin + vDiff;
		target.m_vAngleTo = Aim(G::CurrentUserCmd->viewangles, Math::CalcAngle(vEyePos, target.m_vPos), Vars::Aimbot::General::AimType.Value);

		Vec3 vForward; Math::AngleVectors(target.m_vAngleTo, &vForward);
		Vec3 vTraceEnd = vEyePos + (vForward * flRange);

		SDK::Trace(vEyePos, vTraceEnd, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
		bool bReturn = trace.m_pEnt && trace.m_pEnt == target.m_pEntity;
		if (!bReturn)
		{
			SDK::TraceHull(vEyePos, vTraceEnd, vSwingMins, vSwingMaxs, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
			bReturn = trace.m_pEnt && trace.m_pEnt == target.m_pEntity;
		}

		if (bReturn && Vars::Aimbot::Melee::AutoBackstab.Value && pWeapon->GetWeaponID() == TF_WEAPON_KNIFE)
		{
			if (target.m_TargetType == ETargetType::PLAYER)
				bReturn = CanBackstab(target.m_pEntity, pLocal, target.m_vAngleTo);
			else
				bReturn = false;
		}

		target.m_pEntity->SetAbsOrigin(vRestore);

		if (bReturn)
		{
			target.m_Tick = pTick;
			target.m_bBacktrack = target.m_TargetType == ETargetType::PLAYER /*&& Vars::Backtrack::Enabled.Value*/;

			originTolerance(-0.125f);
			return true;
		}
		else if (Vars::Aimbot::General::AimType.Value == 2)
		{
			auto vAngle = Math::CalcAngle(vEyePos, target.m_vPos);

			Vec3 vForward = Vec3(); Math::AngleVectors(vAngle, &vForward);
			Vec3 vTraceEnd = vEyePos + (vForward * flRange);

			SDK::Trace(vEyePos, vTraceEnd, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);
			if (trace.m_pEnt && trace.m_pEnt == target.m_pEntity)
			{
				originTolerance(-0.125f);
				return 2;
			}
		}
	}

	originTolerance(-0.125f);
	return false;
}



// assume angle calculated outside with other overload
void CAimbotMelee::Aim(CUserCmd* pCmd, Vec3& vAngle)
{
	if (Vars::Aimbot::General::AimType.Value != 3)
	{
		pCmd->viewangles = vAngle;
		I::EngineClient->SetViewAngles(pCmd->viewangles);
	}
	else if (G::IsAttacking)
	{
		SDK::FixMovement(pCmd, vAngle);
		pCmd->viewangles = vAngle;
		G::PSilentAngles = true;
	}
}

Vec3 CAimbotMelee::Aim(Vec3 vCurAngle, Vec3 vToAngle, int iMethod)
{
	Vec3 vReturn = {};

	Math::ClampAngles(vToAngle);

	switch (iMethod)
	{
	case 1: // Plain
	case 3: // Silent
		vReturn = vToAngle;
		break;
	case 2: // Smooth
	{
		auto shortDist = [](const float flAngleA, const float flAngleB)
			{
				const float flDelta = fmodf((flAngleA - flAngleB), 360.f);
				return fmodf(2 * flDelta, 360.f) - flDelta;
			};
		const float t = 1.f - Vars::Aimbot::General::Smoothing.Value / 100.f;
		vReturn.x = vCurAngle.x - shortDist(vCurAngle.x, vToAngle.x) * t;
		vReturn.y = vCurAngle.y - shortDist(vCurAngle.y, vToAngle.y) * t;
		break;
	}
	}

	return vReturn;
}

void CAimbotMelee::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (pWeapon->m_flSmackTime() < 0.f)
		iAimType = Vars::Aimbot::General::AimType.Value;
	else if (iAimType)
		Vars::Aimbot::General::AimType.Value = iAimType;

	if (Vars::Aimbot::General::AimHoldsFire.Value == 2 && !G::CanPrimaryAttack && G::LastUserCmd->buttons & IN_ATTACK && Vars::Aimbot::General::AimType.Value)
		pCmd->buttons |= IN_ATTACK;
	if (!Vars::Aimbot::General::AimType.Value || !G::CanPrimaryAttack && pWeapon->m_flSmackTime() < 0.f)
		return;

	if (RunSapper(pLocal, pWeapon, pCmd))
		return;

	auto targets = SortTargets(pLocal, pWeapon);
	if (targets.empty())
		return;

	iDoubletapTicks = F::Ticks.GetTicks(pLocal);
	const bool bShouldSwing = iDoubletapTicks <= (GetSwingTime(pWeapon) ? 14 : 0) || Vars::CL_Move::Doubletap::AntiWarp.Value && pLocal->m_hGroundEntity();

	Vec3 vEyePos = pLocal->GetShootPos();
	std::unordered_map<CBaseEntity*, std::deque<TickRecord>> pRecordMap;
	std::unordered_map<CBaseEntity*, std::deque<Vec3>> simLines;
	SimulatePlayers(pLocal, pWeapon, targets, vEyePos, pRecordMap, simLines);

	for (auto& target : targets)
	{
		const auto iResult = CanHit(target, pLocal, pWeapon, vEyePos, pRecordMap[target.m_pEntity]);
		if (!iResult) continue;
		if (iResult == 2)
		{
			Aim(pCmd, target.m_vAngleTo);
			break;
		}

		G::Target = { target.m_pEntity->entindex(), I::GlobalVars->tickcount };

		if (Vars::Aimbot::General::AimType.Value == 3)
			G::AimPosition = target.m_vPos;

		if (Vars::Aimbot::General::AutoShoot.Value && pWeapon->m_flSmackTime() < 0.f)
		{
			if (bShouldSwing)
				pCmd->buttons |= IN_ATTACK;
			if (iDoubletapTicks)
				G::DoubleTap = true;
		}

		const bool bAttacking = SDK::IsAttacking(pLocal, pWeapon, pCmd, true);
		G::IsAttacking = bAttacking || bShouldSwing && G::DoubleTap; // dumb but works

		if (G::IsAttacking)
		{
			if (target.m_bBacktrack)
				pCmd->tick_count = TIME_TO_TICKS(target.m_Tick.flSimTime) + TIME_TO_TICKS(F::Backtrack.flFakeInterp);
			// bug: fast old records seem to be progressively more unreliable ?

			if (Vars::Visuals::Bullet::BulletTracer.Value)
			{
				G::BulletsStorage.clear();
				G::BulletsStorage.push_back({ {vEyePos, target.m_vPos}, I::GlobalVars->curtime + 5.f, Vars::Colors::BulletTracer.Value, true });
			}
			if (Vars::Visuals::Hitbox::ShowHitboxes.Value)
			{
				G::BoxesStorage.clear();
				auto vBoxes = F::Visuals.GetHitboxes(target.m_Tick.BoneMatrix.aBones, target.m_pEntity->As<CBaseAnimating>());
				G::BoxesStorage.insert(G::BoxesStorage.end(), vBoxes.begin(), vBoxes.end());
			}
		}
		if (Vars::Visuals::Simulation::SwingLines.Value && pCmd->buttons & IN_ATTACK && pWeapon->m_flSmackTime() < 0.f)
		{
			const bool bAlwaysDraw = !Vars::Aimbot::General::AutoShoot.Value || Vars::Debug::Info.Value;
			if (!bAlwaysDraw)
			{
				G::LinesStorage.clear();
				G::LinesStorage.push_back({ simLines[pLocal], I::GlobalVars->curtime + 5.f, Vars::Colors::ProjectileColor.Value });
				G::LinesStorage.push_back({ simLines[target.m_pEntity], I::GlobalVars->curtime + 5.f, Vars::Colors::PredictionColor.Value });
			}
		}

		Aim(pCmd, target.m_vAngleTo);
		G::IsAttacking = bAttacking;
		break;
	}
}

int GetAttachment(CBaseObject* pBuilding, int i)
{
	int iAttachment = pBuilding->GetBuildPointAttachmentIndex(i);
	if (pBuilding->IsSentrygun() && pBuilding->m_iUpgradeLevel() > 1) // idk why i need this
		iAttachment = 3;
	return iAttachment;
}
bool CAimbotMelee::FindNearestBuildPoint(CBaseObject* pBuilding, CTFPlayer* pLocal, Vec3& vPoint)
{
	bool bFoundPoint = false;

	static auto tf_obj_max_attach_dist = U::ConVars.FindVar("tf_obj_max_attach_dist");
	float flNearestPoint = tf_obj_max_attach_dist ? tf_obj_max_attach_dist->GetFloat() : 160.f;
	for (int i = 0; i < pBuilding->GetNumBuildPoints(); i++)
	{
		int v = GetAttachment(pBuilding, i);

		Vec3 vOrigin;
		if (pBuilding->GetAttachment(v, vOrigin)) // issues using pBuilding->GetBuildPoint i on sentries above level 1 for some reason
		{
			if (!SDK::VisPos(pLocal, pBuilding, pLocal->GetShootPos(), vOrigin))
				continue;

			float flDist = (vOrigin - pLocal->GetAbsOrigin()).Length();
			if (flDist < flNearestPoint)
			{
				flNearestPoint = flDist;
				vPoint = vOrigin;
				bFoundPoint = true;
			}
		}
	}

	return bFoundPoint;
}

bool CAimbotMelee::RunSapper(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	if (pWeapon->GetWeaponID() != TF_WEAPON_BUILDER)
		return false;

	std::vector<Target_t> validTargets;

	const Vec3 vLocalPos = pLocal->GetShootPos();
	const Vec3 vLocalAngles = I::EngineClient->GetViewAngles();
	for (auto pEntity : H::Entities.GetGroup(EGroupType::BUILDINGS_ENEMIES))
	{
		auto pBuilding = pEntity->As<CBaseObject>();
		if (pBuilding->m_bHasSapper() || pBuilding->m_iTeamNum() != 2 && pBuilding->m_iTeamNum() != 3)
			continue;

		Vec3 vPoint;
		if (!FindNearestBuildPoint(pBuilding, pLocal, vPoint))
			continue;

		Vec3 vAngleTo = Math::CalcAngle(vLocalPos, vPoint);
		const float flFOVTo = Math::CalcFov(vLocalAngles, vAngleTo);
		const float flDistTo = vLocalPos.DistTo(vPoint);

		if (flFOVTo > Vars::Aimbot::General::AimFOV.Value)
			continue;

		validTargets.push_back({ pBuilding, ETargetType::UNKNOWN, vPoint, vAngleTo, flFOVTo, flDistTo });
	}

	F::AimbotGlobal.SortTargets(&validTargets, ESortMethod::DISTANCE);
	for (auto& target : validTargets)
	{
		static int iLastRun = 0;

		if ((Vars::Aimbot::General::AimType.Value == 3 ? iLastRun != I::GlobalVars->tickcount - 1 || G::PSilentAngles && G::ShiftedTicks == G::MaxShift : true) && Vars::Aimbot::General::AutoShoot.Value)
			pCmd->buttons |= IN_ATTACK;

		if (pCmd->buttons & IN_ATTACK)
		{
			G::IsAttacking = true;
			target.m_vAngleTo = Aim(pCmd->viewangles, Math::CalcAngle(vLocalPos, target.m_vPos));
			target.m_vAngleTo.x = pCmd->viewangles.x; // we don't need to care about pitch
			Aim(pCmd, target.m_vAngleTo);

			iLastRun = I::GlobalVars->tickcount;
		}

		break;
	}

	return true;
}