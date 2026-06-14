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
	"bytes"
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

// ActionItem / Summary mirror the Python server's SummaryResponse shape.
type ActionItem struct {
	Text  string `json:"text"`
	Owner string `json:"owner,omitempty"`
	Due   string `json:"due,omitempty"`
}

type Summary struct {
	Summary     string       `json:"summary"`
	ActionItems []ActionItem `json:"action_items"`
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
	Summary     *Summary    `json:"summary"`
	Error       any         `json:"error"`
}

type server struct {
	storageDir    string
	maxBytes      int64
	authToken     string // if non-empty, every route except /healthz needs "Authorization: Bearer <token>"
	deepgramKey   string // if non-empty, uploads are transcribed via Deepgram
	deepgramModel string
	anthropicKey  string // if non-empty, transcripts are summarized via Claude
	claudeModel   string
}

func main() {
	port := getenv("PORT", "8000")
	srv := &server{
		storageDir:    getenv("OP_STORAGE_DIR", "/data"),
		maxBytes:      getenvInt64("OP_UPLOAD_MAX_BYTES", 200<<20), // 200 MiB (~50 min of 16k stereo)
		authToken:     os.Getenv("OP_AUTH_TOKEN"),
		deepgramKey:   os.Getenv("DEEPGRAM_API_KEY"),
		deepgramModel: getenv("DEEPGRAM_MODEL", "nova-3"),
		anthropicKey:  os.Getenv("ANTHROPIC_API_KEY"),
		// Default per the claude-api skill; override (e.g. claude-haiku-4-5) to cut cost.
		claudeModel: getenv("CLAUDE_MODEL", "claude-opus-4-8"),
	}
	if err := os.MkdirAll(srv.storageDir, 0o755); err != nil {
		log.Fatalf("cannot create storage dir %s: %v", srv.storageDir, err)
	}
	// Re-enqueue work on boot. Two cases: (1) anything left mid-flight by a
	// restart, and (2) backfill — when a Claude key is newly set, summarize
	// transcripts that already finished `done` without a summary. process()
	// skips stages already complete, so it does the right thing for both.
	if srv.deepgramKey != "" || srv.anthropicKey != "" {
		if recs, err := srv.listMeta(); err == nil {
			for _, rec := range recs {
				pending := rec.Status != "done" && rec.Status != "failed"
				backfill := srv.anthropicKey != "" && rec.Status == "done" &&
					rec.Summary == nil && rec.Transcript != nil &&
					strings.TrimSpace(rec.Transcript.Text) != ""
				if pending || backfill {
					go srv.process(rec)
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
	mux.HandleFunc("GET /v1/recordings/{id}/export.md", srv.handleExport)
	mux.HandleFunc("GET /rec/{id}", srv.handleDetail)
	mux.HandleFunc("GET /", srv.handleIndex)

	addr := ":" + port
	log.Printf("open-plaud listening on %s (storage=%s, max=%d bytes, auth=%v, deepgram=%v, claude=%v)",
		addr, srv.storageDir, srv.maxBytes, srv.authToken != "", srv.deepgramKey != "", srv.anthropicKey != "")
	if err := http.ListenAndServe(addr, logging(srv.auth(mux))); err != nil {
		log.Fatal(err)
	}
}

// auth optionally gates every route except /healthz behind a shared bearer token
// (OP_AUTH_TOKEN). Off by default so the PoC is frictionless; set the env var to
// require "Authorization: Bearer <token>". The device sends it with one
// esp_http_client_set_header call. /healthz stays open for Railway's checks.
func (s *server) auth(next http.Handler) http.Handler {
	match := func(c string) bool {
		// constant-time compare avoids leaking the token via response timing
		return c != "" && subtle.ConstantTimeCompare([]byte(c), []byte(s.authToken)) == 1
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.authToken == "" || r.URL.Path == "/healthz" {
			next.ServeHTTP(w, r)
			return
		}
		// Token may arrive three ways: the Authorization header (devices/curl),
		// a ?token=  query param (first browser visit), or the op_token cookie
		// (browser navigation after the first visit).
		bearer := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		query := r.URL.Query().Get("token")
		ok := match(bearer) || match(query)
		if !ok {
			if c, err := r.Cookie("op_token"); err == nil {
				ok = match(c.Value)
			}
		}
		if !ok {
			httpError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		// On a valid ?token, set a cookie so links and the <audio> element work
		// without repeating the token in every URL, then (for GETs) redirect to
		// the same path with ?token stripped — so the secret doesn't linger in
		// the address bar, history, logs, or a Referer header.
		if match(query) {
			secure := r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https"
			w.Header().Set("Cache-Control", "no-store") // don't let a cache keep the Set-Cookie
			http.SetCookie(w, &http.Cookie{
				Name: "op_token", Value: s.authToken, Path: "/",
				HttpOnly: true, Secure: secure, SameSite: http.SameSiteLaxMode,
				MaxAge: 86400,
			})
			if r.Method == http.MethodGet {
				q := r.URL.Query()
				q.Del("token")
				target := r.URL.Path
				if rq := q.Encode(); rq != "" {
					target += "?" + rq
				}
				http.Redirect(w, r, target, http.StatusSeeOther)
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
		// Process (transcribe + summarize) in the background so the device's POST
		// returns immediately; it polls GET for the result. Mirrors the Python
		// server's BackgroundTask.
		if s.deepgramKey != "" || s.anthropicKey != "" {
			go s.process(rec)
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

// process runs the pipeline for one stored recording and persists each step.
// Runs in a goroutine; status walks received -> transcribing -> summarizing ->
// done (or failed if transcription fails). It's resumable: it skips a stage
// already complete, so the boot-time re-enqueue can finish interrupted work.
// Each writeMeta is atomic (tmp+rename), so a concurrent GET reads a whole record.
func (s *server) process(rec Recording) {
	// 1. Transcribe (Deepgram).
	if rec.Transcript == nil && s.deepgramKey != "" {
		rec.Status = "transcribing"
		if err := s.writeMeta(rec); err != nil {
			log.Printf("process %s: meta: %v", rec.ID, err)
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
		rec.Error = nil
		log.Printf("transcribed %s (%d chars, lang=%q)", rec.ID, len(text), lang)
	}

	// 2. Summarize (Claude). A summary failure is non-fatal — the transcript is
	// still useful, so we finish as done without a summary rather than failing.
	if rec.Summary == nil && s.anthropicKey != "" &&
		rec.Transcript != nil && strings.TrimSpace(rec.Transcript.Text) != "" {
		rec.Status = "summarizing"
		if err := s.writeMeta(rec); err != nil {
			log.Printf("process %s: meta: %v", rec.ID, err)
			return
		}
		sum, err := s.claude(rec.Transcript.Text)
		if err != nil {
			log.Printf("summarize %s: %v", rec.ID, err)
		} else {
			rec.Summary = sum
			log.Printf("summarized %s (%d action items)", rec.ID, len(sum.ActionItems))
		}
	}

	// 3. Done.
	rec.Status = "done"
	if err := s.writeMeta(rec); err != nil {
		log.Printf("process %s: meta: %v", rec.ID, err)
	}
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

// ---- summarization (Claude) -------------------------------------------------

var anthropicClient = &http.Client{Timeout: 120 * time.Second}

// claude summarizes a transcript via the Anthropic Messages API and returns a
// Summary. It asks for a strict JSON object and parses it defensively: if the
// model wraps the JSON in prose or markdown, it still recovers the object, and
// if parsing fails entirely it degrades to using the whole reply as the summary.
func (s *server) claude(transcript string) (*Summary, error) {
	const system = `You summarize voice-note and meeting transcripts. Reply with ONLY a single ` +
		`minified JSON object — no markdown, no code fences, no preamble — matching exactly: ` +
		`{"summary": string, "action_items": [{"text": string, "owner": string, "due": string}]}. ` +
		`"summary" is a concise paragraph. "action_items" lists concrete follow-ups (use [] if none); ` +
		`set "owner"/"due" to "" when unknown. Write the summary in the transcript's own language.`

	reqBody, _ := json.Marshal(map[string]any{
		"model":      s.claudeModel,
		"max_tokens": 4096,
		"system":     system,
		"messages":   []map[string]any{{"role": "user", "content": transcript}},
	})
	req, err := http.NewRequest(http.MethodPost, "https://api.anthropic.com/v1/messages", bytes.NewReader(reqBody))
	if err != nil {
		return nil, err
	}
	req.Header.Set("content-type", "application/json")
	req.Header.Set("x-api-key", s.anthropicKey)
	req.Header.Set("anthropic-version", "2023-06-01")

	resp, err := anthropicClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("anthropic status %d: %s", resp.StatusCode, truncate(string(raw), 300))
	}

	var ar struct {
		StopReason string `json:"stop_reason"`
		Content    []struct {
			Type string `json:"type"`
			Text string `json:"text"`
		} `json:"content"`
	}
	if err := json.Unmarshal(raw, &ar); err != nil {
		return nil, fmt.Errorf("anthropic decode: %w", err)
	}
	if ar.StopReason == "refusal" {
		return nil, fmt.Errorf("claude declined to summarize")
	}
	var text strings.Builder
	for _, b := range ar.Content {
		if b.Type == "text" {
			text.WriteString(b.Text)
		}
	}

	var sum Summary
	if err := json.Unmarshal([]byte(extractJSON(text.String())), &sum); err != nil {
		// Degrade gracefully rather than losing the model's work.
		return &Summary{Summary: strings.TrimSpace(text.String()), ActionItems: []ActionItem{}}, nil
	}
	if sum.ActionItems == nil {
		sum.ActionItems = []ActionItem{}
	}
	return &sum, nil
}

// extractJSON returns the first {...} span, tolerating prose/markdown around it.
func extractJSON(s string) string {
	i := strings.IndexByte(s, '{')
	j := strings.LastIndexByte(s, '}')
	if i >= 0 && j > i {
		return s[i : j+1]
	}
	return s
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
			fmt.Fprintf(&b, `<tr><td class=muted>%s</td><td><a href="/rec/%s">%s</a></td><td>%s</td><td>%s</td><td>%s</td><td><a href="/v1/recordings/%s/audio">download</a></td></tr>`,
				rec.CreatedAt.Format("2006-01-02 15:04:05"),
				rec.ID, html.EscapeString(rec.Filename), humanBytes(rec.SizeBytes), html.EscapeString(rec.Status),
				html.EscapeString(truncate(tx, 160)), rec.ID)
		}
		b.WriteString(`</table>`)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = io.WriteString(w, b.String())
}

// handleDetail renders one recording: audio player, summary + action items, and
// the full transcript. id is validated to be hex by readMeta.
func (s *server) handleDetail(w http.ResponseWriter, r *http.Request) {
	rec, ok := s.readMeta(r.PathValue("id"))
	if !ok {
		httpError(w, http.StatusNotFound, "recording not found")
		return
	}
	var b strings.Builder
	b.WriteString(`<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">`)
	fmt.Fprintf(&b, `<title>%s — open-plaud</title>`, html.EscapeString(rec.Filename))
	b.WriteString(`<style>body{font:15px/1.6 system-ui,sans-serif;max-width:720px;margin:40px auto;padding:0 16px;color:#111}`)
	b.WriteString(`h1{font-size:20px}h2{font-size:15px;margin-top:28px;color:#444}a{color:#0a58ca}.muted{color:#888;font-size:13px}`)
	b.WriteString(`audio{width:100%;margin:8px 0}pre{white-space:pre-wrap;background:#f7f7f5;padding:12px;border-radius:6px}`)
	b.WriteString(`ul{padding-left:20px}.tag{display:inline-block;padding:1px 8px;border-radius:10px;background:#eee;font-size:12px}</style>`)
	b.WriteString(`<p><a href="/">← all recordings</a></p>`)
	fmt.Fprintf(&b, `<h1>%s</h1>`, html.EscapeString(rec.Filename))
	fmt.Fprintf(&b, `<p class=muted>%s · %s · <span class=tag>%s</span></p>`,
		rec.CreatedAt.Format("2006-01-02 15:04:05"), humanBytes(rec.SizeBytes), html.EscapeString(rec.Status))
	fmt.Fprintf(&b, `<audio controls preload=metadata src="/v1/recordings/%s/audio"></audio>`, rec.ID)

	if rec.Summary != nil {
		fmt.Fprintf(&b, `<h2>Summary</h2><p>%s</p>`, html.EscapeString(rec.Summary.Summary))
		if len(rec.Summary.ActionItems) > 0 {
			b.WriteString(`<h2>Action items</h2><ul>`)
			for _, it := range rec.Summary.ActionItems {
				fmt.Fprintf(&b, `<li>%s%s</li>`, html.EscapeString(it.Text), html.EscapeString(itemMeta(it)))
			}
			b.WriteString(`</ul>`)
		}
	}
	switch {
	case rec.Transcript != nil && rec.Transcript.Text != "":
		lang := ""
		if rec.Transcript.Language != "" {
			lang = " (" + html.EscapeString(rec.Transcript.Language) + ")"
		}
		fmt.Fprintf(&b, `<h2>Transcript%s</h2><pre>%s</pre>`, lang, html.EscapeString(rec.Transcript.Text))
	case rec.Status == "done":
		b.WriteString(`<h2>Transcript</h2><p class=muted>No speech detected.</p>`)
	default:
		b.WriteString(`<h2>Transcript</h2><p class=muted>Not available yet — check back after processing.</p>`)
	}
	fmt.Fprintf(&b, `<p style="margin-top:24px"><a href="/v1/recordings/%s/export.md">Export Markdown</a> · <a href="/v1/recordings/%s/audio">Download audio</a></p>`, rec.ID, rec.ID)

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = io.WriteString(w, b.String())
}

func (s *server) handleExport(w http.ResponseWriter, r *http.Request) {
	rec, ok := s.readMeta(r.PathValue("id"))
	if !ok {
		httpError(w, http.StatusNotFound, "recording not found")
		return
	}
	w.Header().Set("Content-Type", "text/markdown; charset=utf-8")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", rec.ID+".md"))
	_, _ = io.WriteString(w, renderMarkdown(rec))
}

func itemMeta(it ActionItem) string {
	var parts []string
	if it.Owner != "" {
		parts = append(parts, "owner: "+it.Owner)
	}
	if it.Due != "" {
		parts = append(parts, "due: "+it.Due)
	}
	if len(parts) == 0 {
		return ""
	}
	return " (" + strings.Join(parts, ", ") + ")"
}

func renderMarkdown(rec Recording) string {
	var b strings.Builder
	fmt.Fprintf(&b, "# %s\n\n", rec.Filename)
	fmt.Fprintf(&b, "- **Status:** %s\n", rec.Status)
	fmt.Fprintf(&b, "- **Created:** %s\n", rec.CreatedAt.Format(time.RFC3339))
	if rec.Summary != nil {
		fmt.Fprintf(&b, "\n## Summary\n\n%s\n", rec.Summary.Summary)
		if len(rec.Summary.ActionItems) > 0 {
			b.WriteString("\n## Action items\n\n")
			for _, it := range rec.Summary.ActionItems {
				fmt.Fprintf(&b, "- [ ] %s%s\n", it.Text, itemMeta(it))
			}
		}
	}
	if rec.Transcript != nil && rec.Transcript.Text != "" {
		fmt.Fprintf(&b, "\n## Transcript\n\n%s\n", rec.Transcript.Text)
	}
	return b.String()
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
