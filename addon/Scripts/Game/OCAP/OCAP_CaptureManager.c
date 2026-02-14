// OCAP_CaptureManager.c — Core capture loop

class OCAP_CaptureManager
{
	protected ref OCAP_TransportService m_Transport;
	protected float m_fCaptureDelay;
	protected ref array<ChimeraCharacter> m_aFrameAICharacters = {};

	void Init(OCAP_TransportService transport, float captureDelay)
	{
		m_Transport = transport;
		m_fCaptureDelay = captureDelay;
	}

	void Start()
	{
		int delayMs = (int)(m_fCaptureDelay * 1000);
		GetGame().GetCallqueue().CallLater(OnCaptureTick, delayMs, true);
		Print("[OCAP] Capture loop started (interval: " + m_fCaptureDelay.ToString() + "s)", LogLevel.NORMAL);
	}

	void Stop()
	{
		GetGame().GetCallqueue().Remove(OnCaptureTick);
		Print("[OCAP] Capture loop stopped", LogLevel.NORMAL);
	}

	protected void OnCaptureTick()
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		string newEntitiesJson = "";
		string unitsJson = "";
		string vehiclesJson = "";
		int newEntityCount = 0;

		// Track which vehicles we've processed this frame to avoid duplicates
		ref set<IEntity> vehiclesSeen = new set<IEntity>();

		// --- Capture player-controlled characters ---
		PlayerManager playerMgr = GetGame().GetPlayerManager();
		array<int> playerIds = {};
		playerMgr.GetPlayers(playerIds);

		foreach (int playerId : playerIds)
		{
			IEntity entity = playerMgr.GetPlayerControlledEntity(playerId);
			if (!entity)
				continue;

			ChimeraCharacter character = ChimeraCharacter.Cast(entity);
			if (!character)
				continue;

			string playerName = playerMgr.GetPlayerName(playerId);

			// Register if new
			if (!session.IsEntityTracked(character))
			{
				string entDef = BuildUnitEntityDef(character, true, playerName);
				if (!entDef.IsEmpty())
				{
					if (newEntityCount > 0) newEntitiesJson += ",";
					newEntitiesJson += entDef;
					newEntityCount++;
				}
			}

			// Capture state
			string unitJson = BuildUnitState(character, true, playerName);
			if (!unitJson.IsEmpty())
			{
				if (!unitsJson.IsEmpty()) unitsJson += ",";
				unitsJson += unitJson;
			}

			// Capture the vehicle this character is in (if any)
			CaptureVehicleFromOccupant(character, vehiclesSeen,
				newEntitiesJson, newEntityCount, vehiclesJson);
		}

		// --- Capture AI characters via world query ---
		m_aFrameAICharacters.Clear();
		GetGame().GetWorld().QueryEntitiesBySphere(
			vector.Zero, 50000,
			QueryAddCharacter,
			null,
			EQueryEntitiesFlags.DYNAMIC
		);

		foreach (ChimeraCharacter aiChar : m_aFrameAICharacters)
		{
			// Skip if already captured as a player
			if (playerMgr.GetPlayerIdFromControlledEntity(aiChar) > 0)
				continue;

			// Register if new
			if (!session.IsEntityTracked(aiChar))
			{
				string entDef = BuildUnitEntityDef(aiChar, false, "");
				if (!entDef.IsEmpty())
				{
					if (newEntityCount > 0) newEntitiesJson += ",";
					newEntitiesJson += entDef;
					newEntityCount++;
				}
			}

			// Capture state
			string unitJson = BuildUnitState(aiChar, false, "");
			if (!unitJson.IsEmpty())
			{
				if (!unitsJson.IsEmpty()) unitsJson += ",";
				unitsJson += unitJson;
			}

			// Capture vehicle
			CaptureVehicleFromOccupant(aiChar, vehiclesSeen,
				newEntitiesJson, newEntityCount, vehiclesJson);
		}

		// --- Send new entities if any ---
		if (newEntityCount > 0)
			m_Transport.SendEntities(session.GetSessionId(), newEntitiesJson);

		// --- Send frame data ---
		m_Transport.SendFrames(session.GetSessionId(), session.GetFrameNum(), unitsJson, vehiclesJson);

		// --- Send queued events ---
		string eventsJson = session.FlushEvents();
		if (!eventsJson.IsEmpty())
			m_Transport.SendEvents(session.GetSessionId(), eventsJson);

		session.IncrementFrame();
	}

	// --- Unit helpers ---

	protected string BuildUnitEntityDef(ChimeraCharacter character, bool isPlayer, string playerName)
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		int id = session.RegisterEntity(character);

		string side = "CIV";
		FactionAffiliationComponent factionComp = FactionAffiliationComponent.Cast(
			character.FindComponent(FactionAffiliationComponent));
		if (factionComp)
		{
			Faction faction = factionComp.GetAffiliatedFaction();
			if (faction)
				side = OCAP_SideMapper.GetSide(faction.GetFactionKey());
		}

		string name = playerName;
		if (name.IsEmpty())
			name = GetCharacterName(character);

		string role = GetCharacterRole(character);
		string group = "";

		int isPlayerInt = 0;
		if (isPlayer) isPlayerInt = 1;

		string json = "{\"id\":" + id.ToString();
		json += ",\"type\":\"unit\"";
		json += ",\"name\":\"" + OCAP_Util.EscapeJson(name) + "\"";
		json += ",\"group\":\"" + OCAP_Util.EscapeJson(group) + "\"";
		json += ",\"side\":\"" + side + "\"";
		json += ",\"isPlayer\":" + isPlayerInt.ToString();
		json += ",\"role\":\"" + OCAP_Util.EscapeJson(role) + "\"";
		json += ",\"startFrameNum\":" + session.GetFrameNum().ToString() + "}";

		return json;
	}

	protected string BuildUnitState(ChimeraCharacter character, bool isPlayer, string playerName)
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		int id = session.GetEntityId(character);
		if (id < 0)
			return "";

		vector pos = character.GetOrigin();
		float bearing = character.GetYawPitchRoll()[0];
		if (bearing < 0) bearing += 360;
		int dir = (int)Math.Round(bearing);

		// Life state
		int lifeState = OCAP_Constants.LIFESTATE_ALIVE;
		CharacterControllerComponent ctrl = CharacterControllerComponent.Cast(
			character.FindComponent(CharacterControllerComponent));
		if (ctrl)
		{
			if (ctrl.IsDead())
				lifeState = OCAP_Constants.LIFESTATE_DEAD;
			else if (ctrl.IsUnconscious())
				lifeState = OCAP_Constants.LIFESTATE_UNCONSCIOUS;
		}

		// Vehicle check
		int vehicleId = 0;
		CompartmentAccessComponent compAccess = CompartmentAccessComponent.Cast(
			character.FindComponent(CompartmentAccessComponent));
		if (compAccess)
		{
			IEntity vehicle = compAccess.GetVehicle();
			if (vehicle)
			{
				int vId = session.GetEntityId(vehicle);
				if (vId >= 0)
					vehicleId = vId;
			}
		}

		// Name
		string name = playerName;
		if (name.IsEmpty())
			name = GetCharacterName(character);

		string role = GetCharacterRole(character);

		int isPlayerInt = 0;
		if (isPlayer) isPlayerInt = 1;

		// Build array: [id, [x,y,z], bearing, lifeState, vehicleId, name, isPlayer, role]
		string json = "[" + id.ToString();
		json += ",[" + pos[0].ToString() + "," + pos[1].ToString() + "," + pos[2].ToString() + "]";
		json += "," + dir.ToString();
		json += "," + lifeState.ToString();
		json += "," + vehicleId.ToString();
		json += ",\"" + OCAP_Util.EscapeJson(name) + "\"";
		json += "," + isPlayerInt.ToString();
		json += ",\"" + OCAP_Util.EscapeJson(role) + "\"]";

		return json;
	}

	// --- Vehicle helpers ---

	protected void CaptureVehicleFromOccupant(ChimeraCharacter character, set<IEntity> vehiclesSeen,
		inout string newEntitiesJson, inout int newEntityCount, inout string vehiclesJson)
	{
		OCAP_Session session = OCAP_Session.GetInstance();

		CompartmentAccessComponent compAccess = CompartmentAccessComponent.Cast(
			character.FindComponent(CompartmentAccessComponent));
		if (!compAccess)
			return;

		IEntity vehicle = compAccess.GetVehicle();
		if (!vehicle || vehiclesSeen.Contains(vehicle))
			return;

		vehiclesSeen.Insert(vehicle);

		// Register if new
		if (!session.IsEntityTracked(vehicle))
		{
			int vId = session.RegisterEntity(vehicle);

			string side = "CIV";
			FactionAffiliationComponent vFactionComp = FactionAffiliationComponent.Cast(
				vehicle.FindComponent(FactionAffiliationComponent));
			if (vFactionComp)
			{
				Faction vFaction = vFactionComp.GetAffiliatedFaction();
				if (vFaction)
					side = OCAP_SideMapper.GetSide(vFaction.GetFactionKey());
			}

			string vName = GetVehicleName(vehicle);
			string vClass = GetVehicleClass(vehicle);

			string entJson = "{\"id\":" + vId.ToString();
			entJson += ",\"type\":\"vehicle\"";
			entJson += ",\"name\":\"" + OCAP_Util.EscapeJson(vName) + "\"";
			entJson += ",\"side\":\"" + side + "\"";
			entJson += ",\"class\":\"" + vClass + "\"";
			entJson += ",\"isPlayer\":0";
			entJson += ",\"startFrameNum\":" + session.GetFrameNum().ToString() + "}";

			if (newEntityCount > 0) newEntitiesJson += ",";
			newEntitiesJson += entJson;
			newEntityCount++;
		}

		// Capture vehicle state
		int vId = session.GetEntityId(vehicle);
		if (vId < 0)
			return;

		vector pos = vehicle.GetOrigin();
		float bearing = vehicle.GetYawPitchRoll()[0];
		if (bearing < 0) bearing += 360;
		int dir = (int)Math.Round(bearing);

		int alive = 1;
		DamageManagerComponent dmg = DamageManagerComponent.Cast(
			vehicle.FindComponent(DamageManagerComponent));
		if (dmg && dmg.IsDestroyed())
			alive = 0;

		// Get crew IDs
		string crewJson = "";
		BaseCompartmentManagerComponent compMgr = BaseCompartmentManagerComponent.Cast(
			vehicle.FindComponent(BaseCompartmentManagerComponent));
		if (compMgr)
		{
			array<BaseCompartmentSlot> slots = {};
			compMgr.GetCompartments(slots);
			bool first = true;
			foreach (BaseCompartmentSlot slot : slots)
			{
				IEntity occupant = slot.GetOccupant();
				if (!occupant)
					continue;
				int occId = session.GetEntityId(occupant);
				if (occId < 0)
					continue;
				if (!first) crewJson += ",";
				crewJson += occId.ToString();
				first = false;
			}
		}

		// Build array: [id, [x,y,z], bearing, alive, [crewIds]]
		string json = "[" + vId.ToString();
		json += ",[" + pos[0].ToString() + "," + pos[1].ToString() + "," + pos[2].ToString() + "]";
		json += "," + dir.ToString();
		json += "," + alive.ToString();
		json += ",[" + crewJson + "]]";

		if (!vehiclesJson.IsEmpty()) vehiclesJson += ",";
		vehiclesJson += json;
	}

	// --- Utility methods ---

	protected string GetCharacterName(ChimeraCharacter character)
	{
		// Try to get display name from identity component or prefab
		SCR_CharacterIdentityComponent identity = SCR_CharacterIdentityComponent.Cast(
			character.FindComponent(SCR_CharacterIdentityComponent));
		if (identity)
		{
			string name = identity.GetCharacterName();
			if (!name.IsEmpty())
				return name;
		}

		// Fallback to prefab name
		EntityPrefabData prefab = character.GetPrefabData();
		if (prefab)
			return prefab.GetPrefabName();
		return "Unknown";
	}

	protected string GetCharacterRole(ChimeraCharacter character)
	{
		// Try to get role from the loadout
		SCR_EditableEntityComponent editable = SCR_EditableEntityComponent.Cast(
			character.FindComponent(SCR_EditableEntityComponent));
		if (editable)
		{
			UIInfo info = editable.GetInfo();
			if (info)
			{
				string name = info.GetName();
				if (!name.IsEmpty())
					return name;
			}
		}

		return "Rifleman";
	}

	protected string GetVehicleName(IEntity vehicle)
	{
		SCR_EditableEntityComponent editable = SCR_EditableEntityComponent.Cast(
			vehicle.FindComponent(SCR_EditableEntityComponent));
		if (editable)
		{
			UIInfo info = editable.GetInfo();
			if (info)
			{
				string name = info.GetName();
				if (!name.IsEmpty())
					return name;
			}
		}

		EntityPrefabData prefab = vehicle.GetPrefabData();
		if (prefab)
			return prefab.GetPrefabName();
		return "Unknown";
	}

	protected string GetVehicleClass(IEntity vehicle)
	{
		// Classify the vehicle based on its components/type
		// Check for helicopter
		if (vehicle.FindComponent(HelicopterControllerComponent))
			return "heli";

		// Check for fixed-wing
		if (vehicle.FindComponent(AircraftControllerComponent))
			return "plane";

		// Check for boat
		if (vehicle.FindComponent(BoatControllerComponent))
			return "sea";

		// Default to car for all ground vehicles
		// A more accurate classification would check the vehicle config
		return "car";
	}

	// Query callback for QueryEntitiesBySphere — collects ChimeraCharacter entities
	protected bool QueryAddCharacter(IEntity entity)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(entity);
		if (character)
			m_aFrameAICharacters.Insert(character);
		return true; // continue enumeration
	}
}
