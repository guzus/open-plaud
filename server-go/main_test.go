package main

import (
	"net/http"
	"net/http/httptest"
	"net/url"
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
