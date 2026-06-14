// open-plaud ingest server (Go, stdlib-only).
//
// A minimal, device-compatible drop-in for the audio-ingest half of the Python
// server. The recorder is "dumb on purpose": it records a WAV locally and POSTs
// the raw audio here. This server stores the blob, exposes it for download, and
// (when DEEPGRAM_API_KEY is set) transcribes it asynchronously via Deepgram.
// Summarization stays out of scope (the full pipeline lives in ../server).
//
// Contract (matches firmware/main/net_upload.c and ../server/open_plaud/api/app.py):
//
//	GET  /healthz                       -> {"status":"ok"}
//	POST /v1/recordings                 -> multipart field "file"; 200 {Recording}
//	GET  /v1/recordings                 -> [Recording, ...]   (newest first)
//	GET  /v1/recordings/{id}            -> {Recording}        (404 if unknown)
//	GET  /v1/recordings/{id}/audio      -> the stored audio blob
//	GET  /                              -> tiny HTML list (for eyeballing the PoC)
//
// Storage is a flat directory (OP_STORAGE_DIR, default /data): each upload is a
// blob "{id}{suffix}" plus a sidecar "{id}.json". No database, no CGO, no deps —
// so the container is a single static binary and the build is trivial. Point
// OP_STORAGE_DIR at a Railway volume to survive redeploys.
package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"html"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// audioSuffixes is the allowlist used both to accept an upload and to derive a
// safe, server-controlled Content-Type on download. We never reflect the
// client's Content-Type unchecked (that would let an upload serve e.g.
// text/html from our origin), mirroring the Python server's _audio_media_type.
var audioSuffixes = map[string]string{
	".wav":  "audio/wav",
	".flac": "audio/flac",
	".m4a":  "audio/mp4",
	".mp3":  "audio/mpeg",
}

var audioMediaTypes = map[string]bool{
	"audio/wav": true, "audio/x-wav": true,
	"audio/flac": true, "audio/mpeg": true, "audio/mp4": true,
}

// Transcript mirrors the Python server's transcript shape (text + language +
// segments). Deepgram pre-recorded gives us text and a detected language;
// segments stay empty (we don't expose word timings here).
type Transcript struct {
	Text     string `json:"text"`
	Language string `json:"language,omitempty"`
	Segments []any  `json:"segments"`
}

// Recording is both the on-disk metadata and the API response shape. The fields
// match the Python RecordingResponse field-for-field so the device poller and
// any existing UI work unchanged. summary stays null (out of scope); transcript
// is filled in asynchronously once Deepgram returns.
type Recording struct {
	ID          string      `json:"id"`
	Status      string      `json:"status"` // received -> transcribing -> done | failed
	Filename    string      `json:"filename"`
	SizeBytes   int64       `json:"size_bytes"`
	CreatedAt   time.Time   `json:"created_at"`
	ContentType string      `json:"content_type,omitempty"`
	StoredName  string      `json:"stored_name,omitempty"`
	Transcript  *Transcript `json:"transcript"`
	Summary     any         `json:"summary"`
	Error       any         `json:"error"`
}

type server struct {
	storageDir    string
	maxBytes      int64
	authToken     string // if non-empty, every route except /healthz needs "Authorization: Bearer <token>"
	deepgramKey   string // if non-empty, uploads are transcribed via Deepgram
	deepgramModel string
}

func main() {
	port := getenv("PORT", "8000")
	srv := &server{
		storageDir:    getenv("OP_STORAGE_DIR", "/data"),
		maxBytes:      getenvInt64("OP_UPLOAD_MAX_BYTES", 200<<20), // 200 MiB (~50 min of 16k stereo)
		authToken:     os.Getenv("OP_AUTH_TOKEN"),
		deepgramKey:   os.Getenv("DEEPGRAM_API_KEY"),
		deepgramModel: getenv("DEEPGRAM_MODEL", "nova-3"),
	}
	if err := os.MkdirAll(srv.storageDir, 0o755); err != nil {
		log.Fatalf("cannot create storage dir %s: %v", srv.storageDir, err)
	}
	// Re-enqueue anything left mid-flight (a restart during transcription, or
	// clips uploaded before the key was set). Background tasks don't survive a
	// restart, so this is the recovery path.
	if srv.deepgramKey != "" {
		if recs, err := srv.listMeta(); err == nil {
			for _, rec := range recs {
				if rec.Transcript == nil && (rec.Status == "received" || rec.Status == "transcribing") {
					go srv.transcribe(rec)
				}
			}
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", srv.handleHealth)
	mux.HandleFunc("POST /v1/recordings", srv.handleUpload)
	mux.HandleFunc("GET /v1/recordings", srv.handleList)
	mux.HandleFunc("GET /v1/recordings/{id}", srv.handleGet)
	mux.HandleFunc("GET /v1/recordings/{id}/audio", srv.handleAudio)
	mux.HandleFunc("GET /", srv.handleIndex)

	addr := ":" + port
	log.Printf("open-plaud ingest listening on %s (storage=%s, max=%d bytes, auth=%v, deepgram=%v)",
		addr, srv.storageDir, srv.maxBytes, srv.authToken != "", srv.deepgramKey != "")
	if err := http.ListenAndServe(addr, logging(srv.auth(mux))); err != nil {
		log.Fatal(err)
	}
}

// auth optionally gates every route except /healthz behind a shared bearer token
// (OP_AUTH_TOKEN). Off by default so the PoC is frictionless; set the env var to
// require "Authorization: Bearer <token>". The device sends it with one
// esp_http_client_set_header call. /healthz stays open for Railway's checks.
func (s *server) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.authToken != "" && r.URL.Path != "/healthz" {
			const prefix = "Bearer "
			got := strings.TrimPrefix(r.Header.Get("Authorization"), prefix)
			// constant-time compare avoids leaking the token via response timing
			if got == r.Header.Get("Authorization") ||
				subtle.ConstantTimeCompare([]byte(got), []byte(s.authToken)) != 1 {
				httpError(w, http.StatusUnauthorized, "unauthorized")
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

// ---- handlers ---------------------------------------------------------------

func (s *server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// handleUpload streams the multipart "file" part straight to disk (no full-body
// buffering), enforces the size cap, and writes a sidecar metadata JSON.
func (s *server) handleUpload(w http.ResponseWriter, r *http.Request) {
	// Backstop the whole body; the per-part LimitReader below gives the precise
	// 413, this just stops a malicious client from streaming forever.
	r.Body = http.MaxBytesReader(w, r.Body, s.maxBytes+(1<<20))

	mr, err := r.MultipartReader()
	if err != nil {
		httpError(w, http.StatusBadRequest, "expected multipart/form-data")
		return
	}

	for {
		part, err := mr.NextPart()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			httpError(w, http.StatusBadRequest, "malformed multipart body")
			return
		}
		if part.FormName() != "file" {
			_ = part.Close()
			continue
		}

		filename := part.FileName()
		if !supportedUpload(filename, part.Header.Get("Content-Type")) {
			httpError(w, http.StatusUnsupportedMediaType, "unsupported audio upload")
			return
		}

		id := newID()
		suffix := safeSuffix(filename)
		storedName := id + suffix
		blobPath := filepath.Join(s.storageDir, storedName)

		out, err := os.Create(blobPath)
		if err != nil {
			log.Printf("create blob: %v", err)
			httpError(w, http.StatusInternalServerError, "cannot store upload")
			return
		}
		// Copy at most maxBytes+1 so we can tell "exactly at limit" from "over".
		size, err := io.Copy(out, io.LimitReader(part, s.maxBytes+1))
		closeErr := out.Close()
		if err != nil {
			_ = os.Remove(blobPath)
			// MaxBytesReader / LimitReader tripping both surface as a read error here.
			httpError(w, http.StatusRequestEntityTooLarge, fmt.Sprintf("upload exceeds %d bytes", s.maxBytes))
			return
		}
		if closeErr != nil {
			_ = os.Remove(blobPath)
			httpError(w, http.StatusInternalServerError, "cannot finalize upload")
			return
		}
		if size > s.maxBytes {
			_ = os.Remove(blobPath)
			httpError(w, http.StatusRequestEntityTooLarge, fmt.Sprintf("upload exceeds %d bytes", s.maxBytes))
			return
		}
		if size == 0 {
			_ = os.Remove(blobPath)
			httpError(w, http.StatusBadRequest, "uploaded file is empty")
			return
		}

		rec := Recording{
			ID:          id,
			Status:      "received",
			Filename:    cleanFilename(filename, storedName),
			SizeBytes:   size,
			CreatedAt:   time.Now().UTC(),
			ContentType: part.Header.Get("Content-Type"),
			StoredName:  storedName,
		}
		if err := s.writeMeta(rec); err != nil {
			_ = os.Remove(blobPath)
			log.Printf("write meta: %v", err)
			httpError(w, http.StatusInternalServerError, "cannot persist metadata")
			return
		}
		log.Printf("received %s (%d bytes) as %s", rec.Filename, size, storedName)
		// Transcribe in the background so the device's POST returns immediately;
		// it polls GET for the transcript. Mirrors the Python server's BackgroundTask.
		if s.deepgramKey != "" {
			go s.transcribe(rec)
		}
		writeJSON(w, http.StatusOK, rec)
		return
	}

	httpError(w, http.StatusBadRequest, `missing "file" field`)
}

func (s *server) handleList(w http.ResponseWriter, _ *http.Request) {
	recs, err := s.listMeta()
	if err != nil {
		log.Printf("list: %v", err)
		httpError(w, http.StatusInternalServerError, "cannot list recordings")
		return
	}
	writeJSON(w, http.StatusOK, recs)
}

func (s *server) handleGet(w http.ResponseWriter, r *http.Request) {
	rec, ok := s.readMeta(r.PathValue("id"))
	if !ok {
		httpError(w, http.StatusNotFound, "recording not found")
		return
	}
	writeJSON(w, http.StatusOK, rec)
}

// handleAudio serves the stored blob. The path is built from the id-keyed
// metadata, never from request input, so there is no user-controlled path to
// traverse; id itself is validated to be hex.
func (s *server) handleAudio(w http.ResponseWriter, r *http.Request) {
	rec, ok := s.readMeta(r.PathValue("id"))
	if !ok {
		httpError(w, http.StatusNotFound, "recording not found")
		return
	}
	blobPath := filepath.Join(s.storageDir, rec.StoredName)
	f, err := os.Open(blobPath)
	if err != nil {
		httpError(w, http.StatusNotFound, "audio not found")
		return
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		httpError(w, http.StatusInternalServerError, "cannot stat audio")
		return
	}
	w.Header().Set("Content-Type", mediaType(rec))
	w.Header().Set("Content-Disposition", fmt.Sprintf("inline; filename=%q", rec.Filename))
	http.ServeContent(w, r, rec.Filename, info.ModTime(), f) // adds Range support for audio scrubbing
}

// ---- transcription (Deepgram) -----------------------------------------------

var dgClient = &http.Client{Timeout: 120 * time.Second}

// transcribe runs Deepgram on one stored recording and persists the result.
// Runs in a goroutine; status walks received -> transcribing -> done | failed.
// Each writeMeta is atomic (tmp+rename), so a concurrent GET always reads a
// complete record.
func (s *server) transcribe(rec Recording) {
	rec.Status = "transcribing"
	if err := s.writeMeta(rec); err != nil {
		log.Printf("transcribe %s: meta: %v", rec.ID, err)
		return
	}
	text, lang, err := s.deepgram(filepath.Join(s.storageDir, rec.StoredName), mediaType(rec))
	if err != nil {
		log.Printf("transcribe %s: %v", rec.ID, err)
		rec.Status = "failed"
		rec.Error = err.Error()
		_ = s.writeMeta(rec)
		return
	}
	rec.Transcript = &Transcript{Text: text, Language: lang, Segments: []any{}}
	rec.Status = "done"
	rec.Error = nil
	if err := s.writeMeta(rec); err != nil {
		log.Printf("transcribe %s: meta: %v", rec.ID, err)
		return
	}
	log.Printf("transcribed %s (%d chars, lang=%q)", rec.ID, len(text), lang)
}

// deepgram POSTs the audio to Deepgram's pre-recorded API and returns the
// best-alternative transcript plus the detected language. Empty speech is not an
// error — it returns an empty transcript.
func (s *server) deepgram(blobPath, contentType string) (string, string, error) {
	f, err := os.Open(blobPath)
	if err != nil {
		return "", "", err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return "", "", err
	}
	q := url.Values{
		"model":           {s.deepgramModel},
		"smart_format":    {"true"},
		"detect_language": {"true"}, // handles Korean/English recordings automatically
	}
	req, err := http.NewRequest(http.MethodPost, "https://api.deepgram.com/v1/listen?"+q.Encode(), f)
	if err != nil {
		return "", "", err
	}
	req.ContentLength = info.Size()
	req.Header.Set("Authorization", "Token "+s.deepgramKey)
	req.Header.Set("Content-Type", contentType)

	resp, err := dgClient.Do(req)
	if err != nil {
		return "", "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", "", fmt.Errorf("deepgram status %d: %s", resp.StatusCode, truncate(string(body), 200))
	}

	var dr struct {
		Results struct {
			Channels []struct {
				DetectedLanguage string `json:"detected_language"`
				Alternatives     []struct {
					Transcript string `json:"transcript"`
				} `json:"alternatives"`
			} `json:"channels"`
		} `json:"results"`
	}
	if err := json.Unmarshal(body, &dr); err != nil {
		return "", "", fmt.Errorf("deepgram decode: %w", err)
	}
	if len(dr.Results.Channels) == 0 || len(dr.Results.Channels[0].Alternatives) == 0 {
		return "", "", nil // no speech detected -> empty transcript
	}
	ch := dr.Results.Channels[0]
	return ch.Alternatives[0].Transcript, ch.DetectedLanguage, nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "…"
}

func (s *server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		httpError(w, http.StatusNotFound, "not found")
		return
	}
	recs, _ := s.listMeta()
	var b strings.Builder
	b.WriteString(`<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">`)
	b.WriteString(`<title>open-plaud ingest</title>`)
	b.WriteString(`<style>body{font:15px/1.5 system-ui,sans-serif;max-width:720px;margin:40px auto;padding:0 16px;color:#111}`)
	b.WriteString(`h1{font-size:20px}table{border-collapse:collapse;width:100%}td,th{padding:6px 8px;border-bottom:1px solid #eee;text-align:left}`)
	b.WriteString(`a{color:#0a58ca}.muted{color:#888}</style>`)
	b.WriteString(`<h1>open-plaud ingest</h1>`)
	if len(recs) == 0 {
		b.WriteString(`<p class=muted>No recordings yet. POST one to <code>/v1/recordings</code> (multipart field <code>file</code>).</p>`)
	} else {
		b.WriteString(`<table><tr><th>when</th><th>filename</th><th>size</th><th>status</th><th>transcript</th><th>audio</th></tr>`)
		for _, rec := range recs {
			tx := ""
			if rec.Transcript != nil {
				tx = rec.Transcript.Text
			}
			fmt.Fprintf(&b, `<tr><td class=muted>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td><a href="/v1/recordings/%s/audio">download</a></td></tr>`,
				rec.CreatedAt.Format("2006-01-02 15:04:05"),
				html.EscapeString(rec.Filename), humanBytes(rec.SizeBytes), html.EscapeString(rec.Status),
				html.EscapeString(truncate(tx, 160)), rec.ID)
		}
		b.WriteString(`</table>`)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = io.WriteString(w, b.String())
}

// ---- storage ----------------------------------------------------------------

func (s *server) metaPath(id string) string { return filepath.Join(s.storageDir, id+".json") }

func (s *server) writeMeta(rec Recording) error {
	data, err := json.MarshalIndent(rec, "", "  ")
	if err != nil {
		return err
	}
	// Write-then-rename so a concurrent list never sees a half-written file.
	tmp := s.metaPath(rec.ID) + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, s.metaPath(rec.ID))
}

func (s *server) readMeta(id string) (Recording, bool) {
	if !validID(id) {
		return Recording{}, false
	}
	data, err := os.ReadFile(s.metaPath(id))
	if err != nil {
		return Recording{}, false
	}
	var rec Recording
	if err := json.Unmarshal(data, &rec); err != nil {
		return Recording{}, false
	}
	return rec, true
}

func (s *server) listMeta() ([]Recording, error) {
	entries, err := os.ReadDir(s.storageDir)
	if err != nil {
		return nil, err
	}
	recs := make([]Recording, 0, len(entries))
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".json") {
			continue
		}
		if rec, ok := s.readMeta(strings.TrimSuffix(name, ".json")); ok {
			recs = append(recs, rec)
		}
	}
	sort.Slice(recs, func(i, j int) bool { return recs[i].CreatedAt.After(recs[j].CreatedAt) })
	return recs, nil
}

// ---- helpers ----------------------------------------------------------------

func supportedUpload(filename, contentType string) bool {
	if _, ok := audioSuffixes[strings.ToLower(filepath.Ext(filename))]; ok {
		return true
	}
	return audioMediaTypes[contentType]
}

func safeSuffix(filename string) string {
	ext := strings.ToLower(filepath.Ext(filename))
	if _, ok := audioSuffixes[ext]; ok {
		return ext
	}
	return ".wav"
}

func mediaType(rec Recording) string {
	if audioMediaTypes[rec.ContentType] {
		return rec.ContentType
	}
	if mt, ok := audioSuffixes[strings.ToLower(filepath.Ext(rec.StoredName))]; ok {
		return mt
	}
	return "application/octet-stream"
}

// cleanFilename keeps only the base name the device sent (for display), falling
// back to the stored name. It is never used to build a filesystem path.
func cleanFilename(filename, fallback string) string {
	base := filepath.Base(strings.ReplaceAll(filename, `\`, "/"))
	if base == "" || base == "." || base == "/" {
		return fallback
	}
	return base
}

func newID() string {
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		// crypto/rand failure is fatal-worthy; fall back to a timestamp so we never
		// collide silently.
		return fmt.Sprintf("ts%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(b[:])
}

func validID(id string) bool {
	if len(id) != 32 {
		return false
	}
	for _, c := range id {
		if (c < '0' || c > '9') && (c < 'a' || c > 'f') {
			return false
		}
	}
	return true
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func httpError(w http.ResponseWriter, code int, detail string) {
	writeJSON(w, code, map[string]string{"detail": detail})
}

func humanBytes(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for x := n / unit; x >= unit; x /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB", float64(n)/float64(div), "KMGTPE"[exp])
}

func logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		log.Printf("%s %s %s", r.Method, r.URL.Path, time.Since(start).Round(time.Millisecond))
	})
}

func getenv(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func getenvInt64(key string, def int64) int64 {
	if v := os.Getenv(key); v != "" {
		var n int64
		if _, err := fmt.Sscan(v, &n); err == nil && n > 0 {
			return n
		}
	}
	return def
}
