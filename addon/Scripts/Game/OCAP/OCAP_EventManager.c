// OCAP_EventManager.c — Event handlers for kills, connections, disconnections

class OCAP_EventManager
{
	protected SCR_BaseGameMode m_GameMode;

	// Registers all event handlers on the game mode
	void Init()
	{
		m_GameMode = SCR_BaseGameMode.Cast(GetGame().GetGameMode());
		if (!m_GameMode)
		{
			Print("[OCAP] EventManager: SCR_BaseGameMode not found, events will not be captured", LogLevel.ERROR);
			return;
		}

		m_GameMode.GetOnPlayerKilled().Insert(OnPlayerKilled);
		m_GameMode.GetOnControllableDestroyed().Insert(OnControllableDestroyed);
		m_GameMode.GetOnPlayerConnected().Insert(OnPlayerConnected);
		m_GameMode.GetOnPlayerDisconnected().Insert(OnPlayerDisconnected);

		Print("[OCAP] EventManager initialized", LogLevel.NORMAL);
	}

	// Removes all event handlers from the game mode
	void Cleanup()
	{
		if (!m_GameMode)
			return;

		m_GameMode.GetOnPlayerKilled().Remove(OnPlayerKilled);
		m_GameMode.GetOnControllableDestroyed().Remove(OnControllableDestroyed);
		m_GameMode.GetOnPlayerConnected().Remove(OnPlayerConnected);
		m_GameMode.GetOnPlayerDisconnected().Remove(OnPlayerDisconnected);

		Print("[OCAP] EventManager cleaned up", LogLevel.NORMAL);
	}

	// --- Kill event handlers ---

	// Handles player-controlled character deaths
	protected void OnPlayerKilled(int playerId, IEntity playerEntity, IEntity killerEntity, notnull Instigator killer)
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		int victimId = session.GetEntityId(playerEntity);
		if (victimId < 0)
			return;

		// Resolve the actual killer entity from the Instigator
		IEntity killerEnt = killer.GetInstigatorEntity();

		int killerId = -1;
		string weaponName = "Unknown";
		float distance = 0;

		if (killerEnt)
		{
			killerId = session.GetEntityId(killerEnt);
			weaponName = GetCurrentWeaponName(killerEnt);
			if (weaponName.IsEmpty())
				weaponName = "Unknown";
			if (playerEntity)
				distance = vector.Distance(playerEntity.GetOrigin(), killerEnt.GetOrigin());
		}

		QueueKillEvent(session, victimId, killerId, weaponName, distance);
	}

	// Handles AI and vehicle destruction. Skips player-controlled entities
	// since those are already handled by OnPlayerKilled.
	protected void OnControllableDestroyed(IEntity entity, IEntity killerEntity, notnull Instigator killer)
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		// Skip player-controlled entities — already handled by OnPlayerKilled
		PlayerManager playerMgr = GetGame().GetPlayerManager();
		if (playerMgr && playerMgr.GetPlayerIdFromControlledEntity(entity) > 0)
			return;

		int victimId = session.GetEntityId(entity);
		if (victimId < 0)
			return;

		// Resolve the actual killer entity from the Instigator
		IEntity killerEnt = killer.GetInstigatorEntity();

		int killerId = -1;
		string weaponName = "Unknown";
		float distance = 0;

		if (killerEnt)
		{
			killerId = session.GetEntityId(killerEnt);
			weaponName = GetCurrentWeaponName(killerEnt);
			if (weaponName.IsEmpty())
				weaponName = "Unknown";
			if (entity)
				distance = vector.Distance(entity.GetOrigin(), killerEnt.GetOrigin());
		}

		QueueKillEvent(session, victimId, killerId, weaponName, distance);
	}

	// Builds and queues a kill event in OCAP2 format:
	// [frameNum, "killed", victimId, [killerId, "weaponName"], distance]
	protected void QueueKillEvent(OCAP_Session session, int victimId, int killerId, string weaponName, float distance)
	{
		string json = "[" + session.GetFrameNum().ToString();
		json += ",\"killed\"";
		json += "," + victimId.ToString();
		json += ",[" + killerId.ToString() + ",\"" + OCAP_Util.EscapeJson(weaponName) + "\"]";
		json += "," + Math.Round(distance).ToString();
		json += "]";

		session.QueueEvent(json);
	}

	// --- Connection event handlers ---

	// Handles player joining the server
	protected void OnPlayerConnected(int playerId)
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		string playerName = GetGame().GetPlayerManager().GetPlayerName(playerId);
		if (playerName.IsEmpty())
			playerName = "Unknown";

		// Format: [frameNum, "connected", "playerName"]
		string json = "[" + session.GetFrameNum().ToString();
		json += ",\"connected\"";
		json += ",\"" + OCAP_Util.EscapeJson(playerName) + "\"";
		json += "]";

		session.QueueEvent(json);
		Print("[OCAP] Player connected: " + playerName, LogLevel.NORMAL);
	}

	// Handles player leaving the server
	protected void OnPlayerDisconnected(int playerId, KickCauseCode cause, int timeout)
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		string playerName = GetGame().GetPlayerManager().GetPlayerName(playerId);
		if (playerName.IsEmpty())
			playerName = "Unknown";

		// Format: [frameNum, "disconnected", "playerName"]
		string json = "[" + session.GetFrameNum().ToString();
		json += ",\"disconnected\"";
		json += ",\"" + OCAP_Util.EscapeJson(playerName) + "\"";
		json += "]";

		session.QueueEvent(json);
		Print("[OCAP] Player disconnected: " + playerName, LogLevel.NORMAL);
	}

	// --- Utility methods ---

	// Retrieves the display name of the entity's currently held weapon.
	// Returns empty string if no weapon is found.
	protected string GetCurrentWeaponName(IEntity entity)
	{
		BaseWeaponManagerComponent weaponMgr = BaseWeaponManagerComponent.Cast(
			entity.FindComponent(BaseWeaponManagerComponent));
		if (!weaponMgr)
			return "";

		BaseWeaponComponent currentWeapon = weaponMgr.GetCurrentWeapon();
		if (!currentWeapon)
			return "";

		UIInfo uiInfo = currentWeapon.GetUIInfo();
		if (!uiInfo)
			return "";

		return uiInfo.GetName();
	}

}
