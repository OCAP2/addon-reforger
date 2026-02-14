package main

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"mime/multipart"
	"net/http"
	"os"
	"strings"
	"time"
)

var uploadClient = &http.Client{Timeout: 60 * time.Second}

const uploadRetries = 3

func CompressAndUpload(export *V1Export, meta StartRequest, endFrame int, cfg Config) error {
	jsonData, err := json.Marshal(export)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}

	var gzBuf bytes.Buffer
	gzWriter := gzip.NewWriter(&gzBuf)
	if _, err := gzWriter.Write(jsonData); err != nil {
		return fmt.Errorf("gzip write: %w", err)
	}
	if err := gzWriter.Close(); err != nil {
		return fmt.Errorf("gzip close: %w", err)
	}

	timestamp := time.Now().Format("20060102_150405")
	safeName := strings.ReplaceAll(meta.MissionName, " ", "_")
	filename := fmt.Sprintf("%s_%s", safeName, timestamp)
	duration := float64(endFrame) * meta.CaptureDelay

	log.Printf("Built export: %s (%d bytes JSON, %d bytes gzipped)", filename, len(jsonData), gzBuf.Len())

	// Retry upload up to 3 times
	var lastErr error
	for attempt := 1; attempt <= uploadRetries; attempt++ {
		lastErr = uploadToOCAP(cfg, filename, meta, duration, gzBuf.Bytes())
		if lastErr == nil {
			return nil
		}
		log.Printf("Upload attempt %d/%d failed: %v", attempt, uploadRetries, lastErr)
		if attempt < uploadRetries {
			time.Sleep(time.Duration(attempt) * 2 * time.Second)
		}
	}

	return lastErr
}

func uploadToOCAP(cfg Config, filename string, meta StartRequest, duration float64, gzData []byte) error {
	if cfg.OcapWebURL == "" {
		return fmt.Errorf("OCAP_WEB_URL not configured")
	}

	var body bytes.Buffer
	writer := multipart.NewWriter(&body)

	writer.WriteField("secret", cfg.OcapAPISecret)
	writer.WriteField("filename", filename)
	writer.WriteField("worldName", meta.WorldName)
	writer.WriteField("missionName", meta.MissionName)
	writer.WriteField("missionDuration", fmt.Sprintf("%.1f", duration))
	writer.WriteField("tag", meta.Tag)

	part, err := writer.CreateFormFile("file", filename+".json.gz")
	if err != nil {
		return fmt.Errorf("create form file: %w", err)
	}
	if _, err := part.Write(gzData); err != nil {
		return fmt.Errorf("write form file: %w", err)
	}
	writer.Close()

	url := strings.TrimRight(cfg.OcapWebURL, "/") + "/api/v1/operations/add"
	req, err := http.NewRequest("POST", url, &body)
	if err != nil {
		return fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("Content-Type", writer.FormDataContentType())

	resp, err := uploadClient.Do(req)
	if err != nil {
		return fmt.Errorf("upload: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 300 {
		respBody, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("upload returned %d: %s", resp.StatusCode, string(respBody))
	}

	log.Printf("Uploaded %s to OCAP web server (%d bytes gzipped)", filename, len(gzData))
	return nil
}

func SaveToDisk(export *V1Export, meta StartRequest) error {
	jsonData, err := json.Marshal(export)
	if err != nil {
		return fmt.Errorf("marshal: %w", err)
	}

	timestamp := time.Now().Format("20060102_150405")
	safeName := strings.ReplaceAll(meta.MissionName, " ", "_")
	filename := fmt.Sprintf("fallback_%s_%s.json.gz", safeName, timestamp)

	var gzBuf bytes.Buffer
	gzWriter := gzip.NewWriter(&gzBuf)
	gzWriter.Write(jsonData)
	gzWriter.Close()

	if err := os.WriteFile(filename, gzBuf.Bytes(), 0644); err != nil {
		return fmt.Errorf("write file: %w", err)
	}

	log.Printf("Saved fallback file: %s (%d bytes)", filename, gzBuf.Len())
	return nil
}
