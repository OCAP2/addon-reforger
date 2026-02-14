// OCAP_GameModeComponent.c — Entry point, wires all OCAP components together

[ComponentEditorProps(category: "GameScripted/OCAP", description: "OCAP Recording Component")]
class OCAP_GameModeComponentClass : SCR_BaseGameModeComponentClass
{
}

class OCAP_GameModeComponent : SCR_BaseGameModeComponent
{
	[Attribute("1", UIWidgets.CheckBox, "Enable OCAP recording")]
	protected bool m_bEnabled;

	[Attribute("1.0", UIWidgets.Slider, "Capture interval in seconds", "0.5 2.0 0.1")]
	protected float m_fCaptureDelay;

	[Attribute("http://localhost:8080", UIWidgets.EditBox, "Go receiver URL")]
	protected string m_sReceiverUrl;

	[Attribute("1", UIWidgets.CheckBox, "Auto-start recording on mission begin")]
	protected bool m_bAutoStart;

	[Attribute("1", UIWidgets.Slider, "Minimum players required for auto-start", "0 64 1")]
	protected int m_iMinPlayerCount;

	[Attribute("", UIWidgets.EditBox, "Mission tag (e.g. TvT, COOP)")]
	protected string m_sTag;

	protected ref OCAP_TransportService m_Transport;
	protected ref OCAP_CaptureManager m_CaptureManager;
	protected ref OCAP_EventManager m_EventManager;

	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		if (!Replication.IsServer())
			return;

		if (!m_bEnabled)
		{
			Print("[OCAP] Recording disabled via settings", LogLevel.NORMAL);
			return;
		}

		// Initialize transport
		m_Transport = new OCAP_TransportService();
		m_Transport.Init(m_sReceiverUrl);

		// Initialize capture manager
		m_CaptureManager = new OCAP_CaptureManager();
		m_CaptureManager.Init(m_Transport, m_fCaptureDelay);

		// Initialize event manager
		m_EventManager = new OCAP_EventManager();
		m_EventManager.Init();

		Print("[OCAP] GameModeComponent initialized", LogLevel.NORMAL);

		if (m_bAutoStart)
		{
			GetGame().GetCallqueue().CallLater(TryAutoStart, 10000, true);
			Print("[OCAP] Auto-start enabled, checking every 10s for " + m_iMinPlayerCount.ToString() + " player(s)", LogLevel.NORMAL);
		}
	}

	protected void TryAutoStart()
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (session.IsRecording() || session.IsWaitingForSession())
		{
			GetGame().GetCallqueue().Remove(TryAutoStart);
			return;
		}

		// Check player count
		PlayerManager playerMgr = GetGame().GetPlayerManager();
		if (!playerMgr)
			return;

		array<int> playerIds = {};
		playerMgr.GetPlayers(playerIds);

		if (playerIds.Count() >= m_iMinPlayerCount)
		{
			GetGame().GetCallqueue().Remove(TryAutoStart);
			Print("[OCAP] Auto-start: player threshold met (" + playerIds.Count().ToString() + "/" + m_iMinPlayerCount.ToString() + ")", LogLevel.NORMAL);
			StartRecording();
		}
	}

	void StartRecording()
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		session.StartRecording();

		// Get world/mission info
		string worldName = GetGame().GetWorldFile();
		string missionName = "";
		string missionAuthor = "";

		MissionHeader header = GetGame().GetMissionHeader();
		if (header)
		{
			missionName = header.m_sName;
			missionAuthor = header.m_sAuthor;
		}

		if (missionName.IsEmpty())
			missionName = worldName;

		// Send start request to receiver
		m_Transport.SendStart(worldName, missionName, missionAuthor, m_fCaptureDelay, m_sTag);

		// Start capture loop
		m_CaptureManager.Start();

		Print("[OCAP] Recording started — world: " + worldName + ", mission: " + missionName, LogLevel.NORMAL);
	}

	void StopRecording()
	{
		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		// Stop capture loop
		m_CaptureManager.Stop();

		// Queue end mission event: [frameNum, "endMission", ["UNKNOWN", "Mission ended"]]
		string eventJson = "[" + session.GetFrameNum().ToString();
		eventJson += ",\"endMission\"";
		eventJson += ",[\"UNKNOWN\",\"Mission ended\"]";
		eventJson += "]";
		session.QueueEvent(eventJson);

		// Flush and send remaining events
		string eventsJson = session.FlushEvents();
		if (!eventsJson.IsEmpty())
			m_Transport.SendEvents(session.GetSessionId(), eventsJson);

		// Send end request
		m_Transport.SendEnd(session.GetSessionId(), session.GetFrameNum(), "missionEnd");

		// Update session state
		session.StopRecording();

		Print("[OCAP] Recording stopped", LogLevel.NORMAL);
	}

	override void OnGameModeEnd(SCR_GameModeEndData endData)
	{
		super.OnGameModeEnd(endData);
		StopRecording();
	}

	override void OnPlayerDisconnected(int playerId, KickCauseCode cause, int timeout)
	{
		super.OnPlayerDisconnected(playerId, cause, timeout);

		OCAP_Session session = OCAP_Session.GetInstance();
		if (!session.IsRecording())
			return;

		// Auto-stop if player count drops to 1 or below
		// Note: the disconnecting player is still counted, so check <= 2
		PlayerManager playerMgr = GetGame().GetPlayerManager();
		if (!playerMgr)
			return;

		array<int> playerIds = {};
		playerMgr.GetPlayers(playerIds);

		if (playerIds.Count() <= 2)
		{
			Print("[OCAP] Player count dropped to " + (playerIds.Count() - 1).ToString() + ", auto-stopping recording", LogLevel.NORMAL);
			StopRecording();
		}
	}

	void ~OCAP_GameModeComponent()
	{
		if (m_EventManager)
			m_EventManager.Cleanup();

		StopRecording();
	}
}
