// OCAP_TransportService.c — HTTP transport via RestApi

class OCAP_RestCallback : RestCallback
{
	protected string m_sEndpoint;

	void OCAP_RestCallback(string endpoint)
	{
		m_sEndpoint = endpoint;
	}

	override void OnSuccess(string data, int dataSize)
	{
		// Success — no action needed for most endpoints
	}

	override void OnError(int errorCode)
	{
		Print("[OCAP] REST error on " + m_sEndpoint + ": " + errorCode.ToString(), LogLevel.ERROR);
	}

	override void OnTimeout()
	{
		Print("[OCAP] REST timeout on " + m_sEndpoint, LogLevel.WARNING);
	}
}

class OCAP_StartCallback : RestCallback
{
	override void OnSuccess(string data, int dataSize)
	{
		// Parse sessionId from response: {"sessionId":"uuid"}
		int start = data.IndexOf("\"sessionId\":\"");
		if (start < 0)
		{
			Print("[OCAP] Failed to parse session start response: " + data, LogLevel.ERROR);
			return;
		}
		start += 14; // length of "sessionId":"
		int end = data.IndexOfFrom(start, "\"");
		if (end < 0)
		{
			Print("[OCAP] Failed to parse sessionId end quote: " + data, LogLevel.ERROR);
			return;
		}

		string sessionId = data.Substring(start, end - start);
		OCAP_Session session = OCAP_Session.GetInstance();
		if (session)
			session.OnSessionStarted(sessionId);
	}

	override void OnError(int errorCode)
	{
		Print("[OCAP] Failed to start session: error " + errorCode.ToString(), LogLevel.ERROR);
	}

	override void OnTimeout()
	{
		Print("[OCAP] Session start request timed out", LogLevel.WARNING);
	}
}

class OCAP_TransportService
{
	protected ref RestContext m_Ctx;
	protected string m_sBaseUrl;

	void Init(string baseUrl)
	{
		m_sBaseUrl = baseUrl;
		m_Ctx = GetGame().GetRestApi().GetContext(baseUrl);
		m_Ctx.SetHeaders("Content-Type,application/json");
		Print("[OCAP] TransportService initialized: " + baseUrl, LogLevel.NORMAL);
	}

	void SendStart(string worldName, string missionName, string missionAuthor, float captureDelay, string tag)
	{
		string body = "{";
		body += "\"worldName\":\"" + OCAP_Util.EscapeJson(worldName) + "\",";
		body += "\"missionName\":\"" + OCAP_Util.EscapeJson(missionName) + "\",";
		body += "\"missionAuthor\":\"" + OCAP_Util.EscapeJson(missionAuthor) + "\",";
		body += "\"captureDelay\":" + captureDelay.ToString() + ",";
		body += "\"tag\":\"" + OCAP_Util.EscapeJson(tag) + "\"";
		body += "}";

		m_Ctx.POST(new OCAP_StartCallback(), "/api/session/start", body);
	}

	void SendEntities(string sessionId, string entitiesJson)
	{
		string body = "{\"sessionId\":\"" + sessionId + "\",\"entities\":[" + entitiesJson + "]}";
		m_Ctx.POST(new OCAP_RestCallback("/api/session/entities"), "/api/session/entities", body);
	}

	void SendFrames(string sessionId, int frameNum, string unitsJson, string vehiclesJson)
	{
		string body = "{\"sessionId\":\"" + sessionId + "\",\"frameNum\":" + frameNum.ToString();
		body += ",\"units\":[" + unitsJson + "]";
		body += ",\"vehicles\":[" + vehiclesJson + "]}";
		m_Ctx.POST(new OCAP_RestCallback("/api/session/frames"), "/api/session/frames", body);
	}

	void SendEvents(string sessionId, string eventsJson)
	{
		if (eventsJson.IsEmpty())
			return;
		string body = "{\"sessionId\":\"" + sessionId + "\",\"events\":[" + eventsJson + "]}";
		m_Ctx.POST(new OCAP_RestCallback("/api/session/events"), "/api/session/events", body);
	}

	void SendEnd(string sessionId, int endFrame, string reason)
	{
		string body = "{\"sessionId\":\"" + sessionId + "\",\"endFrame\":" + endFrame.ToString();
		body += ",\"endReason\":\"" + reason + "\"}";
		m_Ctx.POST(new OCAP_RestCallback("/api/session/end"), "/api/session/end", body);
	}
}
