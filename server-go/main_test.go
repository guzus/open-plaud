package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"net/textproto"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func authedTestHandler(s *server) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok"))
	})
	mux.HandleFunc("GET /v1/recordings", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("api ok"))
	})
	return s.auth(mux)
}

func TestAuthRejectsQueryToken(t *testing.T) {
	s := &server{authToken: "secret"}
	h := authedTestHandler(s)

	req := httptest.NewRequest(http.MethodGet, "/?token=secret", nil)
	req.Header.Set("Accept", "text/html")
	rr := httptest.NewRecorder()

	h.ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("GET with query token status = %d, want %d", rr.Code, http.StatusUnauthorized)
	}
	if got := rr.Header().Values("Set-Cookie"); len(got) != 0 {
		t.Fatalf("GET with query token set cookies: %v", got)
	}
	if !strings.Contains(rr.Body.String(), `type=password`) {
		t.Fatalf("GET with query token did not render password form: %q", rr.Body.String())
	}
}

func TestLoginSetsSessionCookieAndStripsTokenFromRedirect(t *testing.T) {
	s := &server{authToken: "secret"}
	h := authedTestHandler(s)

	form := url.Values{
		"password": {"secret"},
		"next":     {"/rec/abc?token=secret&x=1"},
	}
	req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	rr := httptest.NewRecorder()

	h.ServeHTTP(rr, req)

	if rr.Code != http.StatusSeeOther {
		t.Fatalf("POST /login status = %d, want %d", rr.Code, http.StatusSeeOther)
	}
	if got := rr.Header().Get("Location"); got != "/rec/abc?x=1" {
		t.Fatalf("POST /login Location = %q, want %q", got, "/rec/abc?x=1")
	}

	var sessionCookie *http.Cookie
	for _, cookie := range rr.Result().Cookies() {
		if cookie.Name == authSessionCookieName {
			sessionCookie = cookie
			break
		}
	}
	if sessionCookie == nil {
		t.Fatal("POST /login did not set session cookie")
	}
	if sessionCookie.Value == "secret" {
		t.Fatal("session cookie leaked raw password")
	}

	authReq := httptest.NewRequest(http.MethodGet, "/", nil)
	authReq.AddCookie(sessionCookie)
	authRR := httptest.NewRecorder()
	h.ServeHTTP(authRR, authReq)

	if authRR.Code != http.StatusOK {
		t.Fatalf("GET / with session cookie status = %d, want %d", authRR.Code, http.StatusOK)
	}
}

func TestBearerAuthStillWorks(t *testing.T) {
	s := &server{authToken: "secret"}
	h := authedTestHandler(s)

	req := httptest.NewRequest(http.MethodGet, "/v1/recordings", nil)
	req.Header.Set("Authorization", "Bearer secret")
	rr := httptest.NewRecorder()

	h.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("GET /v1/recordings with bearer status = %d, want %d", rr.Code, http.StatusOK)
	}
}

func TestAPIUnauthorizedReturnsJSON(t *testing.T) {
	s := &server{authToken: "secret"}
	h := authedTestHandler(s)

	req := httptest.NewRequest(http.MethodGet, "/v1/recordings", nil)
	req.Header.Set("Accept", "application/json")
	rr := httptest.NewRecorder()

	h.ServeHTTP(rr, req)

	if rr.Code != http.StatusUnauthorized {
		t.Fatalf("GET /v1/recordings status = %d, want %d", rr.Code, http.StatusUnauthorized)
	}
	if got := rr.Header().Get("Content-Type"); got != "application/json" {
		t.Fatalf("Content-Type = %q, want application/json", got)
	}
	if got := strings.TrimSpace(rr.Body.String()); got != `{"detail":"unauthorized"}` {
		t.Fatalf("body = %q, want unauthorized JSON", got)
	}
}

func TestCompressedADPCMUploadStoresDecodedWAV(t *testing.T) {
	s := &server{storageDir: t.TempDir(), maxBytes: 1 << 20}
	req := multipartUploadRequest(t, "clip.opadpcm", openPlaudADPCMContentType, tinyOpenPlaudADPCM(t))
	rr := httptest.NewRecorder()

	s.handleUpload(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("POST compressed status = %d, body = %q", rr.Code, rr.Body.String())
	}
	var rec Recording
	if err := json.Unmarshal(rr.Body.Bytes(), &rec); err != nil {
		t.Fatalf("decode recording: %v", err)
	}
	if rec.ContentType != "audio/wav" {
		t.Fatalf("ContentType = %q, want audio/wav", rec.ContentType)
	}
	if rec.Filename != "clip.wav" {
		t.Fatalf("Filename = %q, want clip.wav", rec.Filename)
	}
	if !strings.HasSuffix(rec.StoredName, ".wav") {
		t.Fatalf("StoredName = %q, want .wav suffix", rec.StoredName)
	}

	got, err := os.ReadFile(filepath.Join(s.storageDir, rec.StoredName))
	if err != nil {
		t.Fatalf("read stored wav: %v", err)
	}
	want := tinyPCM16WAV(t, 8000, 1, []int16{0, 0, 0, 0})
	if !bytes.Equal(got, want) {
		t.Fatalf("decoded WAV mismatch:\ngot  %x\nwant %x", got, want)
	}
	if rec.SizeBytes != int64(len(want)) {
		t.Fatalf("SizeBytes = %d, want %d", rec.SizeBytes, len(want))
	}
}

func TestOggOpusUploadStoresEncodedBlob(t *testing.T) {
	s := &server{storageDir: t.TempDir(), maxBytes: 1 << 20}
	original := []byte("OggS\x00\x02tiny-opus-placeholder")
	req := multipartUploadRequest(t, "clip.opus", "audio/ogg", original)
	rr := httptest.NewRecorder()

	s.handleUpload(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("POST opus status = %d, body = %q", rr.Code, rr.Body.String())
	}
	var rec Recording
	if err := json.Unmarshal(rr.Body.Bytes(), &rec); err != nil {
		t.Fatalf("decode recording: %v", err)
	}
	if rec.ContentType != "audio/ogg" {
		t.Fatalf("ContentType = %q, want audio/ogg", rec.ContentType)
	}
	if rec.Filename != "clip.opus" {
		t.Fatalf("Filename = %q, want clip.opus", rec.Filename)
	}
	if !strings.HasSuffix(rec.StoredName, ".opus") {
		t.Fatalf("StoredName = %q, want .opus suffix", rec.StoredName)
	}
	stored, err := os.ReadFile(filepath.Join(s.storageDir, rec.StoredName))
	if err != nil {
		t.Fatalf("read stored opus: %v", err)
	}
	if !bytes.Equal(stored, original) {
		t.Fatal("opus upload was modified")
	}
}

func TestNormalWAVUploadCompatibility(t *testing.T) {
	s := &server{storageDir: t.TempDir(), maxBytes: 1 << 20}
	original := tinyPCM16WAV(t, 16000, 1, []int16{0, 512, -512})

	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/recordings", s.handleUpload)
	mux.HandleFunc("GET /v1/recordings/{id}/audio", s.handleAudio)

	post := multipartUploadRequest(t, "normal.wav", "audio/wav", original)
	post.URL.Path = "/v1/recordings"
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, post)

	if rr.Code != http.StatusOK {
		t.Fatalf("POST wav status = %d, body = %q", rr.Code, rr.Body.String())
	}
	var rec Recording
	if err := json.Unmarshal(rr.Body.Bytes(), &rec); err != nil {
		t.Fatalf("decode recording: %v", err)
	}
	if rec.ContentType != "audio/wav" || rec.Filename != "normal.wav" {
		t.Fatalf("recording metadata = content_type %q filename %q", rec.ContentType, rec.Filename)
	}
	stored, err := os.ReadFile(filepath.Join(s.storageDir, rec.StoredName))
	if err != nil {
		t.Fatalf("read stored wav: %v", err)
	}
	if !bytes.Equal(stored, original) {
		t.Fatal("normal WAV upload was modified")
	}

	get := httptest.NewRequest(http.MethodGet, "/v1/recordings/"+rec.ID+"/audio", nil)
	audioRR := httptest.NewRecorder()
	mux.ServeHTTP(audioRR, get)

	if audioRR.Code != http.StatusOK {
		t.Fatalf("GET audio status = %d", audioRR.Code)
	}
	if got := audioRR.Header().Get("Content-Type"); got != "audio/wav" {
		t.Fatalf("audio Content-Type = %q, want audio/wav", got)
	}
	if !bytes.Equal(audioRR.Body.Bytes(), original) {
		t.Fatal("downloaded WAV differs from uploaded WAV")
	}
}

func multipartUploadRequest(t *testing.T, filename, contentType string, payload []byte) *http.Request {
	t.Helper()
	var body bytes.Buffer
	mw := multipart.NewWriter(&body)
	h := make(textproto.MIMEHeader)
	h.Set("Content-Disposition", `form-data; name="file"; filename="`+filename+`"`)
	h.Set("Content-Type", contentType)
	part, err := mw.CreatePart(h)
	if err != nil {
		t.Fatalf("create multipart file: %v", err)
	}
	if _, err := part.Write(payload); err != nil {
		t.Fatalf("write multipart file: %v", err)
	}
	if err := mw.Close(); err != nil {
		t.Fatalf("close multipart: %v", err)
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/recordings", &body)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	return req
}

func tinyOpenPlaudADPCM(t *testing.T) []byte {
	t.Helper()
	var b bytes.Buffer
	_, _ = b.WriteString(openPlaudADPCMMagic)
	mustWriteBinary(t, &b, uint32(8000))
	mustWriteBinary(t, &b, uint16(1))
	mustWriteBinary(t, &b, uint32(4))
	mustWriteBinary(t, &b, int16(0))
	_ = b.WriteByte(0) // IMA step index
	_ = b.WriteByte(0) // reserved
	_, _ = b.Write([]byte{0x00, 0x00})
	return b.Bytes()
}

func tinyPCM16WAV(t *testing.T, sampleRate uint32, channels uint16, samples []int16) []byte {
	t.Helper()
	var b bytes.Buffer
	if err := writeWAVHeader(&b, sampleRate, channels, uint32(len(samples)*2)); err != nil {
		t.Fatalf("write wav header: %v", err)
	}
	for _, sample := range samples {
		mustWriteBinary(t, &b, sample)
	}
	return b.Bytes()
}

func mustWriteBinary(t *testing.T, w io.Writer, v any) {
	t.Helper()
	if err := binary.Write(w, binary.LittleEndian, v); err != nil {
		t.Fatalf("binary write: %v", err)
	}
}
