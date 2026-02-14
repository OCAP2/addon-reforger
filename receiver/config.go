package main

import "os"

type Config struct {
	ListenAddr     string
	OcapWebURL     string
	OcapAPISecret  string
	SessionTimeout string
}

func LoadConfig() Config {
	return Config{
		ListenAddr:     envOr("LISTEN_ADDR", ":8080"),
		OcapWebURL:     envOr("OCAP_WEB_URL", "http://localhost:5000"),
		OcapAPISecret:  envOr("OCAP_API_SECRET", ""),
		SessionTimeout: envOr("SESSION_TIMEOUT", "3h"),
	}
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
