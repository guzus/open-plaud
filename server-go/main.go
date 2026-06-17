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
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"
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

const (
	openPlaudADPCMSuffix      = ".opadpcm"
	openPlaudADPCMContentType = "application/vnd.openplaud.adpcm"
	openPlaudADPCMMagic       = "OPADPCM1"
)

const (
	authSessionCookieName = "op_session"
	authCookieMaxAge      = 86400
)

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
	storageDir     string
	maxBytes       int64
	authToken      string // if non-empty, every route except /healthz needs bearer auth or browser login
	deepgramKey    string // if non-empty, uploads are transcribed via Deepgram
	deepgramModel  string
	fireworksKey   string // if non-empty, transcripts are summarized via Fireworks (Kimi)
	fireworksModel string
}

func main() {
	port := getenv("PORT", "8000")
	srv := &server{
		storageDir:     getenv("OP_STORAGE_DIR", "/data"),
		maxBytes:       getenvInt64("OP_UPLOAD_MAX_BYTES", 200<<20), // 200 MiB (~50 min of 16k stereo)
		authToken:      os.Getenv("OP_AUTH_TOKEN"),
		deepgramKey:    os.Getenv("DEEPGRAM_API_KEY"),
		deepgramModel:  getenv("DEEPGRAM_MODEL", "nova-3"),
		fireworksKey:   os.Getenv("FIREWORKS_API_KEY"),
		fireworksModel: getenv("FIREWORKS_MODEL", "accounts/fireworks/models/kimi-k2p7-code"),
	}
	if err := os.MkdirAll(srv.storageDir, 0o755); err != nil {
		log.Fatalf("cannot create storage dir %s: %v", srv.storageDir, err)
	}
	// Re-enqueue work on boot. Two cases: (1) anything left mid-flight by a
	// restart, and (2) backfill — when a Claude key is newly set, summarize
	// transcripts that already finished `done` without a summary. process()
	// skips stages already complete, so it does the right thing for both.
	if srv.deepgramKey != "" || srv.fireworksKey != "" {
		if recs, err := srv.listMeta(); err == nil {
			for _, rec := range recs {
				pending := rec.Status != "done" && rec.Status != "failed"
				backfill := srv.fireworksKey != "" && rec.Status == "done" &&
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
	log.Printf("open-plaud listening on %s (storage=%s, max=%d bytes, auth=%v, deepgram=%v, summarize=%v)",
		addr, srv.storageDir, srv.maxBytes, srv.authToken != "", srv.deepgramKey != "", srv.fireworksKey != "")
	if err := http.ListenAndServe(addr, logging(srv.auth(mux))); err != nil {
		log.Fatal(err)
	}
}

// auth optionally gates every route except /healthz behind a shared secret
// (OP_AUTH_TOKEN). API/device callers use "Authorization: Bearer <token>".
// Browsers POST the same secret to /login and receive an HttpOnly session cookie.
// /healthz stays open for Railway's checks.
func (s *server) auth(next http.Handler) http.Handler {
	matchSecret := func(c string) bool {
		// constant-time compare avoids leaking the token via response timing
		return c != "" && subtle.ConstantTimeCompare([]byte(c), []byte(s.authToken)) == 1
	}
	matchSession := func(c string) bool {
		return c != "" && subtle.ConstantTimeCompare([]byte(c), []byte(s.authSessionValue())) == 1
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.authToken == "" || r.URL.Path == "/healthz" {
			next.ServeHTTP(w, r)
			return
		}
		if r.URL.Path == "/login" {
			s.handleLogin(w, r, matchSecret, matchSession)
			return
		}

		// Token may arrive via the Authorization header (devices/curl) or via the
		// browser session cookie set by /login. Query-string tokens are deliberately
		// not accepted so secrets do not land in URLs, browser history, logs, or
		// Referer headers.
		bearer := strings.TrimSpace(strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer "))
		ok := matchSecret(bearer)
		if !ok {
			if c, err := r.Cookie(authSessionCookieName); err == nil {
				ok = matchSession(c.Value)
			}
		}
		if !ok {
			if shouldRenderLogin(r) {
				s.writeLoginForm(w, http.StatusUnauthorized, "", r.URL.RequestURI())
				return
			}
			httpError(w, http.StatusUnauthorized, "unauthorized")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *server) handleLogin(
	w http.ResponseWriter,
	r *http.Request,
	matchSecret func(string) bool,
	matchSession func(string) bool,
) {
	switch r.Method {
	case http.MethodGet, http.MethodHead:
		if c, err := r.Cookie(authSessionCookieName); err == nil && matchSession(c.Value) {
			http.Redirect(w, r, loginRedirectTarget(r.URL.Query().Get("next")), http.StatusSeeOther)
			return
		}
		s.writeLoginForm(w, http.StatusOK, "", r.URL.Query().Get("next"))
	case http.MethodPost:
		if err := r.ParseForm(); err != nil {
			s.writeLoginForm(w, http.StatusBadRequest, "Could not read password.", r.FormValue("next"))
			return
		}
		next := r.FormValue("next")
		if !matchSecret(r.FormValue("password")) {
			s.writeLoginForm(w, http.StatusUnauthorized, "Invalid password.", next)
			return
		}
		s.setAuthSessionCookie(w, r)
		http.Redirect(w, r, loginRedirectTarget(next), http.StatusSeeOther)
	default:
		w.Header().Set("Allow", "GET, POST")
		httpError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}

func shouldRenderLogin(r *http.Request) bool {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		return false
	}
	if strings.HasPrefix(r.URL.Path, "/v1/") {
		return false
	}
	accept := r.Header.Get("Accept")
	return accept == "" || strings.Contains(accept, "text/html") || strings.Contains(accept, "*/*")
}

func (s *server) writeLoginForm(w http.ResponseWriter, code int, message, next string) {
	next = loginRedirectTarget(next)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(code)

	var b strings.Builder
	b.WriteString(`<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">`)
	b.WriteString(`<title>open-plaud login</title>`)
	b.WriteString(`<style>body{font:15px/1.5 system-ui,sans-serif;max-width:360px;margin:64px auto;padding:0 16px;color:#111}`)
	b.WriteString(`h1{font-size:20px;margin-bottom:20px}label{display:block;margin-bottom:6px;color:#444}`)
	b.WriteString(`input{box-sizing:border-box;width:100%;font:inherit;padding:10px 12px;border:1px solid #ccc;border-radius:6px}`)
	b.WriteString(`button{font:inherit;margin-top:12px;padding:10px 14px;border:0;border-radius:6px;background:#111;color:white;cursor:pointer}`)
	b.WriteString(`.error{color:#b00020;margin:0 0 12px}</style>`)
	b.WriteString(`<h1>open-plaud</h1>`)
	if message != "" {
		fmt.Fprintf(&b, `<p class=error>%s</p>`, html.EscapeString(message))
	}
	fmt.Fprintf(&b, `<form method=post action="/login"><input type=hidden name=next value="%s">`, html.EscapeString(next))
	b.WriteString(`<label for=password>Password</label>`)
	b.WriteString(`<input id=password name=password type=password autocomplete=current-password autofocus required>`)
	b.WriteString(`<button type=submit>Sign in</button></form>`)
	_, _ = io.WriteString(w, b.String())
}

func (s *server) setAuthSessionCookie(w http.ResponseWriter, r *http.Request) {
	secure := r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https"
	w.Header().Set("Cache-Control", "no-store")
	http.SetCookie(w, &http.Cookie{
		Name:     authSessionCookieName,
		Value:    s.authSessionValue(),
		Path:     "/",
		HttpOnly: true,
		Secure:   secure,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   authCookieMaxAge,
	})
	http.SetCookie(w, &http.Cookie{
		Name:     "op_token",
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   secure,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   -1,
	})
}

func (s *server) authSessionValue() string {
	sum := sha256.Sum256([]byte("open-plaud auth session\x00" + s.authToken))
	return hex.EncodeToString(sum[:])
}

func loginRedirectTarget(raw string) string {
	if raw == "" {
		return "/"
	}
	u, err := url.Parse(raw)
	if err != nil || u.IsAbs() || u.Host != "" || u.Path == "" || u.Path == "/login" {
		return "/"
	}
	if !strings.HasPrefix(raw, "/") || strings.HasPrefix(raw, "//") {
		return "/"
	}
	q := u.Query()
	if q.Has("token") {
		q.Del("token")
		u.RawQuery = q.Encode()
	}
	return u.RequestURI()
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
		contentType := part.Header.Get("Content-Type")
		compressed := isOpenPlaudADPCMUpload(filename, contentType)
		suffix := safeSuffix(filename)
		if compressed {
			suffix = ".wav"
			contentType = "audio/wav"
		}
		storedName := id + suffix
		blobPath := filepath.Join(s.storageDir, storedName)

		out, err := os.Create(blobPath)
		if err != nil {
			log.Printf("create blob: %v", err)
			httpError(w, http.StatusInternalServerError, "cannot store upload")
			return
		}
		var size int64
		if compressed {
			size, err = decodeOpenPlaudADPCMToWAV(out, part, s.maxBytes, s.maxBytes)
		} else {
			// Copy at most maxBytes+1 so we can tell "exactly at limit" from "over".
			size, err = io.Copy(out, io.LimitReader(part, s.maxBytes+1))
		}
		closeErr := out.Close()
		if err != nil {
			_ = os.Remove(blobPath)
			if errors.Is(err, errUploadTooLarge) || !compressed {
				httpError(w, http.StatusRequestEntityTooLarge, fmt.Sprintf("upload exceeds %d bytes", s.maxBytes))
			} else {
				httpError(w, http.StatusBadRequest, err.Error())
			}
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
			Filename:    cleanFilename(normalizedFilename(filename, compressed), storedName),
			SizeBytes:   size,
			CreatedAt:   time.Now().UTC(),
			ContentType: contentType,
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
		if s.deepgramKey != "" || s.fireworksKey != "" {
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
	if rec.Summary == nil && s.fireworksKey != "" &&
		rec.Transcript != nil && strings.TrimSpace(rec.Transcript.Text) != "" {
		rec.Status = "summarizing"
		if err := s.writeMeta(rec); err != nil {
			log.Printf("process %s: meta: %v", rec.ID, err)
			return
		}
		sum, err := s.summarize(rec.Transcript.Text)
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

// ---- summarization (Fireworks / Kimi) ---------------------------------------

var fireworksClient = &http.Client{Timeout: 120 * time.Second}

// summarize sends a transcript to the Fireworks chat-completions API (OpenAI-
// compatible) and returns a Summary. response_format json_object forces the
// model to emit a clean JSON object (Kimi otherwise prepends reasoning); we
// still parse defensively and degrade to raw text if needed.
func (s *server) summarize(transcript string) (*Summary, error) {
	const system = `You summarize voice-note and meeting transcripts. Output ONLY a JSON object: ` +
		`{"summary": string, "action_items": [{"text": string, "owner": string, "due": string}]}. ` +
		`"summary" is a concise paragraph. "action_items" are concrete follow-ups (use [] if none); ` +
		`set "owner"/"due" to "" when unknown. Write the summary in the transcript's own language.`

	reqBody, _ := json.Marshal(map[string]any{
		"model":           s.fireworksModel,
		"max_tokens":      2048,
		"response_format": map[string]any{"type": "json_object"},
		"messages": []map[string]any{
			{"role": "system", "content": system},
			{"role": "user", "content": transcript},
		},
	})
	req, err := http.NewRequest(http.MethodPost,
		"https://api.fireworks.ai/inference/v1/chat/completions", bytes.NewReader(reqBody))
	if err != nil {
		return nil, err
	}
	req.Header.Set("content-type", "application/json")
	req.Header.Set("authorization", "Bearer "+s.fireworksKey)

	resp, err := fireworksClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("fireworks status %d: %s", resp.StatusCode, truncate(string(raw), 300))
	}

	var fr struct {
		Choices []struct {
			Message struct {
				Content string `json:"content"`
			} `json:"message"`
		} `json:"choices"`
	}
	if err := json.Unmarshal(raw, &fr); err != nil {
		return nil, fmt.Errorf("fireworks decode: %w", err)
	}
	if len(fr.Choices) == 0 {
		return nil, fmt.Errorf("fireworks: empty choices")
	}
	content := fr.Choices[0].Message.Content

	var sum Summary
	if err := json.Unmarshal([]byte(extractJSON(content)), &sum); err != nil {
		// Degrade gracefully rather than losing the model's work.
		return &Summary{Summary: strings.TrimSpace(content), ActionItems: []ActionItem{}}, nil
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

// ---- Open Plaud ADPCM ingest ------------------------------------------------

var errUploadTooLarge = errors.New("upload too large")

type countingReader struct {
	r io.Reader
	n int64
}

func (r *countingReader) Read(p []byte) (int, error) {
	n, err := r.r.Read(p)
	r.n += int64(n)
	return n, err
}

type countingWriter struct {
	w io.Writer
	n int64
}

func (w *countingWriter) Write(p []byte) (int, error) {
	n, err := w.w.Write(p)
	w.n += int64(n)
	return n, err
}

type adpcmNibbleReader struct {
	r       io.Reader
	current byte
	high    bool
}

func (r *adpcmNibbleReader) next() (byte, error) {
	if !r.high {
		var b [1]byte
		if _, err := io.ReadFull(r.r, b[:]); err != nil {
			return 0, err
		}
		r.current = b[0]
		r.high = true
		return r.current & 0x0f, nil
	}
	r.high = false
	return r.current >> 4, nil
}

type adpcmState struct {
	predictor int
	index     int
}

// decodeOpenPlaudADPCMToWAV reads the dependency-free Open Plaud ADPCM
// container and writes canonical 16-bit little-endian PCM WAV. The container is:
//
//	8 bytes   "OPADPCM1"
//	uint32    sample rate, little-endian
//	uint16    channel count (1 or 2)
//	uint32    samples per channel, including the predictor sample
//	repeated per channel:
//	  int16   initial predictor PCM sample
//	  uint8   initial IMA step index
//	  uint8   reserved, must be 0
//	nibbles   IMA ADPCM samples, low nibble first, interleaved by frame/channel
func decodeOpenPlaudADPCMToWAV(dst io.Writer, src io.Reader, maxInputBytes, maxOutputBytes int64) (int64, error) {
	lr := &io.LimitedReader{R: src, N: maxInputBytes + 1}
	cr := &countingReader{r: lr}

	var magic [8]byte
	if _, err := io.ReadFull(cr, magic[:]); err != nil {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM header: %w", err)
	}
	if string(magic[:]) != openPlaudADPCMMagic {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM magic")
	}

	var sampleRate uint32
	var channels uint16
	var samplesPerChannel uint32
	if err := binary.Read(cr, binary.LittleEndian, &sampleRate); err != nil {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM sample rate: %w", err)
	}
	if err := binary.Read(cr, binary.LittleEndian, &channels); err != nil {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM channels: %w", err)
	}
	if err := binary.Read(cr, binary.LittleEndian, &samplesPerChannel); err != nil {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM sample count: %w", err)
	}
	if sampleRate == 0 || sampleRate > 192000 {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM sample rate %d", sampleRate)
	}
	if channels == 0 || channels > 2 {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM channel count %d", channels)
	}
	if samplesPerChannel == 0 {
		return 0, fmt.Errorf("invalid Open Plaud ADPCM empty audio")
	}

	dataBytes := int64(samplesPerChannel) * int64(channels) * 2
	if dataBytes > int64(^uint32(0))-36 || dataBytes+44 > maxOutputBytes {
		return 0, errUploadTooLarge
	}

	states := make([]adpcmState, int(channels))
	for i := range states {
		var predictor int16
		var state [2]byte
		if err := binary.Read(cr, binary.LittleEndian, &predictor); err != nil {
			return 0, fmt.Errorf("invalid Open Plaud ADPCM predictor: %w", err)
		}
		if _, err := io.ReadFull(cr, state[:]); err != nil {
			return 0, fmt.Errorf("invalid Open Plaud ADPCM state: %w", err)
		}
		if state[0] >= byte(len(imaStepTable)) {
			return 0, fmt.Errorf("invalid Open Plaud ADPCM step index %d", state[0])
		}
		if state[1] != 0 {
			return 0, fmt.Errorf("invalid Open Plaud ADPCM reserved byte")
		}
		states[i] = adpcmState{predictor: int(predictor), index: int(state[0])}
	}

	cw := &countingWriter{w: dst}
	if err := writeWAVHeader(cw, sampleRate, channels, uint32(dataBytes)); err != nil {
		return cw.n, err
	}
	for ch := range states {
		if err := binary.Write(cw, binary.LittleEndian, int16(states[ch].predictor)); err != nil {
			return cw.n, err
		}
	}

	nibbles := &adpcmNibbleReader{r: cr}
	for sample := uint32(1); sample < samplesPerChannel; sample++ {
		for ch := range states {
			code, err := nibbles.next()
			if err != nil {
				return cw.n, fmt.Errorf("truncated Open Plaud ADPCM payload: %w", err)
			}
			pcm := decodeIMANibble(&states[ch], code)
			if err := binary.Write(cw, binary.LittleEndian, int16(pcm)); err != nil {
				return cw.n, err
			}
		}
	}

	trailing, err := io.Copy(io.Discard, cr)
	if cr.n > maxInputBytes {
		return cw.n, errUploadTooLarge
	}
	if err != nil {
		return cw.n, err
	}
	if trailing > 0 {
		return cw.n, fmt.Errorf("invalid Open Plaud ADPCM trailing data")
	}
	return cw.n, nil
}

func writeWAVHeader(w io.Writer, sampleRate uint32, channels uint16, dataBytes uint32) error {
	byteRate := sampleRate * uint32(channels) * 2
	blockAlign := channels * 2
	header := make([]byte, 44)
	copy(header[0:4], "RIFF")
	binary.LittleEndian.PutUint32(header[4:8], 36+dataBytes)
	copy(header[8:12], "WAVE")
	copy(header[12:16], "fmt ")
	binary.LittleEndian.PutUint32(header[16:20], 16)
	binary.LittleEndian.PutUint16(header[20:22], 1)
	binary.LittleEndian.PutUint16(header[22:24], channels)
	binary.LittleEndian.PutUint32(header[24:28], sampleRate)
	binary.LittleEndian.PutUint32(header[28:32], byteRate)
	binary.LittleEndian.PutUint16(header[32:34], blockAlign)
	binary.LittleEndian.PutUint16(header[34:36], 16)
	copy(header[36:40], "data")
	binary.LittleEndian.PutUint32(header[40:44], dataBytes)
	_, err := w.Write(header)
	return err
}

func decodeIMANibble(s *adpcmState, code byte) int {
	step := imaStepTable[s.index]
	diff := step >> 3
	if code&1 != 0 {
		diff += step >> 2
	}
	if code&2 != 0 {
		diff += step >> 1
	}
	if code&4 != 0 {
		diff += step
	}
	if code&8 != 0 {
		s.predictor -= diff
	} else {
		s.predictor += diff
	}
	if s.predictor > 32767 {
		s.predictor = 32767
	} else if s.predictor < -32768 {
		s.predictor = -32768
	}
	s.index += imaIndexTable[code&0x0f]
	if s.index < 0 {
		s.index = 0
	} else if s.index >= len(imaStepTable) {
		s.index = len(imaStepTable) - 1
	}
	return s.predictor
}

var imaIndexTable = [16]int{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
}

var imaStepTable = [89]int{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
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
	if isOpenPlaudADPCMUpload(filename, contentType) {
		return true
	}
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

func isOpenPlaudADPCMUpload(filename, contentType string) bool {
	return strings.EqualFold(filepath.Ext(filename), openPlaudADPCMSuffix) ||
		strings.EqualFold(contentType, openPlaudADPCMContentType)
}

func normalizedFilename(filename string, compressed bool) string {
	if !compressed {
		return filename
	}
	ext := filepath.Ext(filename)
	if ext == "" {
		return filename + ".wav"
	}
	return strings.TrimSuffix(filename, ext) + ".wav"
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
