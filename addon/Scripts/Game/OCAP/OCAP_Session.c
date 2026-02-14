// OCAP_Session.c — Singleton session state

class OCAP_Session
{
	private static ref OCAP_Session s_Instance;

	protected bool m_bRecording;
	protected bool m_bWaitingForSession;
	protected string m_sSessionId;
	protected int m_iFrameNum;
	protected int m_iNextEntityId;
	protected ref map<IEntity, int> m_mEntityIds;
	protected ref array<string> m_aPendingEvents;

	void OCAP_Session()
	{
		m_bRecording = false;
		m_bWaitingForSession = false;
		m_sSessionId = "";
		m_iFrameNum = 0;
		m_iNextEntityId = 0;
		m_mEntityIds = new map<IEntity, int>();
		m_aPendingEvents = {};
	}

	static OCAP_Session GetInstance()
	{
		if (!s_Instance)
			s_Instance = new OCAP_Session();
		return s_Instance;
	}

	bool IsRecording()
	{
		return m_bRecording && !m_sSessionId.IsEmpty();
	}

	bool IsWaitingForSession()
	{
		return m_bWaitingForSession;
	}

	void OnSessionStarted(string sessionId)
	{
		m_sSessionId = sessionId;
		m_bWaitingForSession = false;
		m_bRecording = true;
		Print("[OCAP] Session started: " + sessionId, LogLevel.NORMAL);
	}

	void StartRecording()
	{
		m_bWaitingForSession = true;
		m_iFrameNum = 0;
		m_iNextEntityId = 0;
		m_mEntityIds.Clear();
		m_aPendingEvents.Clear();
	}

	void StopRecording()
	{
		m_bRecording = false;
		m_bWaitingForSession = false;
		m_sSessionId = "";
	}

	string GetSessionId()
	{
		return m_sSessionId;
	}

	int GetFrameNum()
	{
		return m_iFrameNum;
	}

	void IncrementFrame()
	{
		m_iFrameNum++;
	}

	// Returns the OCAP ID for an entity, or -1 if not tracked
	int GetEntityId(IEntity entity)
	{
		if (!entity || !m_mEntityIds.Contains(entity))
			return -1;
		return m_mEntityIds.Get(entity);
	}

	// Assigns a new OCAP ID to an entity. Returns the new ID.
	int RegisterEntity(IEntity entity)
	{
		int id = m_iNextEntityId;
		m_mEntityIds.Set(entity, id);
		m_iNextEntityId++;
		return id;
	}

	bool IsEntityTracked(IEntity entity)
	{
		return entity && m_mEntityIds.Contains(entity);
	}

	void QueueEvent(string eventJson)
	{
		m_aPendingEvents.Insert(eventJson);
	}

	// Returns all pending events as comma-separated JSON and clears the queue
	string FlushEvents()
	{
		if (m_aPendingEvents.IsEmpty())
			return "";

		string result = "";
		for (int i = 0; i < m_aPendingEvents.Count(); i++)
		{
			if (i > 0)
				result += ",";
			result += m_aPendingEvents[i];
		}
		m_aPendingEvents.Clear();
		return result;
	}
}
