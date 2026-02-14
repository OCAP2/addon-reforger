// OCAP_Types.c — Enums, constants, faction-to-side mapping

class OCAP_Constants
{
	// Life states matching OCAP2 v1 format
	static const int LIFESTATE_DEAD = 0;
	static const int LIFESTATE_ALIVE = 1;
	static const int LIFESTATE_UNCONSCIOUS = 2;
}

class OCAP_Util
{
	static string EscapeJson(string input)
	{
		string result = input;
		result = result.Replace("\\", "\\\\");
		result = result.Replace("\"", "\\\"");
		result = result.Replace("\n", "\\n");
		result = result.Replace("\r", "\\r");
		result = result.Replace("\t", "\\t");
		return result;
	}
}

class OCAP_SideMapper
{
	// Maps Reforger faction keys to OCAP2 side strings.
	// Default Reforger factions: "US", "USSR", "FIA"
	// This can be extended for modded factions.
	static string GetSide(string factionKey)
	{
		if (factionKey == "US" || factionKey == "USA")
			return "WEST";
		if (factionKey == "USSR" || factionKey == "RUSSIA")
			return "EAST";
		if (factionKey == "FIA")
			return "GUER";
		return "CIV";
	}
}
