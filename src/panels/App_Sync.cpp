// Sync review translation unit: the right-side SESSION panel plus the per-frame
// pump that applies inbound host events (spectator) and broadcasts changed
// transport state (host). Networking itself lives in SyncSession; this file is
// the App-side glue. See the "Sync review" block in App.h.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "Project.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace jplay;

namespace {
const SDL_Color& kBg          = kPanelBg; // shared window-chrome fill (#121214)
constexpr SDL_Color kEdge     { 12, 12, 14, 255 };
constexpr SDL_Color kLabel    { 130, 135, 148, 255 };
constexpr SDL_Color kValue    { 215, 218, 225, 255 };
constexpr SDL_Color kHead     { 160, 170, 200, 255 };
constexpr SDL_Color kBtnBg    { 44, 46, 52, 255 };
constexpr SDL_Color kBtnHi    { 62, 74, 104, 255 };
constexpr SDL_Color kBtnEdge  { 90, 94, 104, 255 };
constexpr SDL_Color kAccent   { 100, 120, 180, 255 };
constexpr SDL_Color kHostOn   { 90, 170, 110, 255 };
constexpr SDL_Color kFieldBg  { 20, 21, 24, 255 };

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
} // namespace

// ── per-frame pump ──────────────────────────────────────────────────────────

void App::drainSync() {
    // Master switch (SESSION panel, persisted): with networking off no socket is
    // ever opened. Turning it off tears the existing ones down in
    // toggleSyncNetwork(), so there is nothing left to pump here.
    if (!syncNetwork_)
        return;

    // Discovery runs while browsing: either the SESSION panel is open, or the
    // start-window launcher is up (which is what makes its SYNC SESSION tab
    // available at all). Not in a session.
    if ((panelOpen(kPanelSync) || launcherVisible()) && syncSession_.role() == syncreview::Role::None)
        syncSession_.startDiscovery();
    else
        syncSession_.stopDiscovery();

    // Spectator: the host dropped — release control back to free viewing.
    if (syncSession_.role() == syncreview::Role::Spectator && syncSession_.hostLost()) {
        syncSession_.leave();
        setStatus("SESSION HOST LOST — CONTROL RELEASED", 5000);
        return;
    }

    for (auto& m : syncSession_.drain()) {
        switch (m.type) {
        case syncreview::MsgType::Project:
            loadProjectFromBuffer(m.blob);
            break;
        case syncreview::MsgType::Play:
            setPlayhead(m.i64);
            playing_ = true;
            playDir_ = 1;
            playAcc_ = 0.0;
            break;
        case syncreview::MsgType::Pause:
            playing_ = false;
            setPlayhead(m.i64);
            break;
        case syncreview::MsgType::Seek:
            setPlayhead(m.i64);
            break;
        case syncreview::MsgType::Sequence:
            setSequenceView(m.i32); // moves the playhead; a following Seek corrects it
            break;
        case syncreview::MsgType::Matte:
            // Host drives the matte. Sent ahead of View in the same tick, so the
            // zoom/pan that follows decodes against the fit the host encoded it
            // with (Fit View fits the masked region, not the whole image).
            setLetterboxRatio(m.ratio);
            timeline_.letterboxOpacity = m.opacity;
            letterboxFitView_ = m.fitView;
            break;
        case syncreview::MsgType::View:
            // Host drives zoom/pan; mirror it onto this spectator's frame view.
            applyFrameView(m.zoom, m.u, m.v);
            break;
        case syncreview::MsgType::Exposure:
            // Host drives the player's exposure too, so everyone is judging the
            // same image. The rest of the grade stays local.
            grade_.gain = m.gain;
            grade_.gamma = m.gamma;
            expBypassed_ = false; // the local stash means nothing now
            markGradeDirty();
            break;
        case syncreview::MsgType::Stroke: {
            // Host only: a spectator's completed stroke. Decode one stroke
            // [i64 frame][3f rgb][u32 n][n * 3f pt], merge it into this frame's
            // authoritative set, then re-broadcast the combined set to all viewers.
            const std::string& b = m.blob;
            size_t off = 0;
            auto get = [&](void* p, size_t n) -> bool {
                if (off + n > b.size()) return false;
                std::memcpy(p, b.data() + off, n);
                off += n;
                return true;
            };
            int64_t frame = 0;
            uint32_t n = 0;
            AnnotStroke st;
            if (!get(&frame, 8) || !get(&st.r, 4) || !get(&st.g, 4) || !get(&st.b, 4) ||
                !get(&n, 4) || n > 100000)
                break;
            bool ok = true;
            st.pts.reserve(n);
            for (uint32_t i = 0; i < n; ++i) {
                AnnotPt pt{};
                if (!get(&pt.x, 4) || !get(&pt.y, 4) || !get(&pt.hw, 4)) { ok = false; break; }
                st.pts.push_back(pt);
            }
            if (!ok) break;
            // The host is authoritative on the frame: ignore markup for any frame it
            // isn't parked on. The live buffer already holds this frame's set.
            if (frame != timeline_.playhead) break;
            // Keep a local in-progress stroke (always at back()) as the last entry.
            if (annotDrawing_ && !annotStrokes_.empty())
                annotStrokes_.insert(annotStrokes_.end() - 1, std::move(st));
            else
                annotStrokes_.push_back(std::move(st));
            flushAnnotBuffer();  // persist the merged set onto the clip
            broadcastAnnotations();
            break;
        }
        case syncreview::MsgType::Annotations: {
            // Spectator only: the host's combined stroke set for a frame. Replace
            // local markup wholesale. Layout: [i64 frame][u32 count][count strokes],
            // each stroke [3f rgb][u32 n][n * 3f pt].
            const std::string& b = m.blob;
            size_t off = 0;
            auto get = [&](void* p, size_t n) -> bool {
                if (off + n > b.size()) return false;
                std::memcpy(p, b.data() + off, n);
                off += n;
                return true;
            };
            int64_t frame = 0;
            uint32_t count = 0;
            if (!get(&frame, 8) || !get(&count, 4) || count > 100000)
                break;
            std::vector<AnnotStroke> incoming;
            incoming.reserve(count);
            bool ok = true;
            for (uint32_t c = 0; c < count && ok; ++c) {
                AnnotStroke st;
                uint32_t n = 0;
                if (!get(&st.r, 4) || !get(&st.g, 4) || !get(&st.b, 4) ||
                    !get(&n, 4) || n > 100000) { ok = false; break; }
                st.pts.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    AnnotPt pt{};
                    if (!get(&pt.x, 4) || !get(&pt.y, 4) || !get(&pt.hw, 4)) { ok = false; break; }
                    st.pts.push_back(pt);
                }
                if (ok) incoming.push_back(std::move(st));
            }
            if (!ok) break;
            // The host set is authoritative for the frame it's parked on; a preceding
            // Seek keeps spectator and host aligned, so this replaces the current
            // frame's buffer. Ignore a set for any other frame (out-of-order guard).
            if (frame != timeline_.playhead) break;
            // Point the live buffer at the current frame so the render-time reconcile
            // doesn't flush this incoming set back onto the frame we just left.
            { int64_t sf = 0; Clip* pc = clipAtSourceFrame(timeline_.playhead, sf);
              annotClipId_ = pc ? pc->id : -1; annotFrame_ = sf; }
            // Preserve a stroke this spectator is mid-drawing (kept at back()) so a
            // concurrent update from another peer doesn't erase it under the pen.
            AnnotStroke inProgress;
            bool keep = annotDrawing_ && !annotStrokes_.empty();
            if (keep) inProgress = std::move(annotStrokes_.back());
            annotStrokes_ = std::move(incoming);
            if (keep) annotStrokes_.push_back(std::move(inProgress));
            flushAnnotBuffer();  // persist so scrubbing back to this frame restores it
            break;
        }
        case syncreview::MsgType::Clear:
            // Erase the target frame's stored markup wherever it lives.
            { int64_t sf = 0; if (Clip* pc = clipAtSourceFrame(m.i64, sf)) pc->annotations.erase(sf); }
            if (m.i64 == timeline_.playhead) {
                annotStrokes_.clear();
                annotDrawing_ = false;
                int64_t sf = 0; Clip* pc = clipAtSourceFrame(timeline_.playhead, sf);
                annotClipId_ = pc ? pc->id : -1; annotFrame_ = sf;
            }
            // Host relays a spectator's clear request out to every viewer.
            if (syncSession_.role() == syncreview::Role::Host)
                syncSession_.sendClear(m.i64);
            break;
        }
    }

    if (syncSession_.role() == syncreview::Role::Host)
        broadcastHostState();
}

void App::broadcastHostState() {
    // A structural edit since the last frame (add/remove source, add sequence)
    // means the spectators' project is stale — re-push the whole snapshot. Done
    // before the transport diffs so viewers have the new clips before any seek.
    if (hostSnapshotDirty_) {
        refreshHostSnapshot();
        hostSnapshotDirty_ = false;
    }
    // A scratch view — a source view or the Layout stage's comparison stack — is
    // the host's own detour, and one the snapshot spectators hold knows nothing
    // about (Project::saveTo skips the temporary sequence). Neither the index it
    // puts viewSeqIdx_ on nor the frames it parks the playhead on mean anything on
    // the other end, so the scope and transport diffs are held for its duration:
    // the last-sent values stay put, and closing the view — which restores the
    // exact scope and frame it was opened from — leaves nothing to re-send.
    // Everything below (zoom, exposure, matte) is a view setting rather than a
    // position, so it keeps flowing.
    const bool sourceDetour = scratchActive();
    // Sequence switch first, so a trailing Seek can correct the playhead the
    // spectator's setSequenceView moved.
    if (!sourceDetour && (int32_t)viewSeqIdx_ != lastSentSeq_) {
        syncSession_.sendSequence((int32_t)viewSeqIdx_);
        lastSentSeq_ = (int32_t)viewSeqIdx_;
    }
    // Play/pause transitions carry the frame; while paused (and not mid-scrub) a
    // changed playhead is a seek. During playback we emit nothing — spectators
    // free-run and re-align on the next pause (v1: pause/seek-only sync).
    if (sourceDetour) {
        // held; see above
    } else if (playing_ != lastSentPlaying_) {
        if (playing_)
            syncSession_.sendPlay(timeline_.playhead);
        else
            syncSession_.sendPause(timeline_.playhead);
        lastSentPlaying_ = playing_;
        lastSentFrame_ = timeline_.playhead;
    } else if (!playing_ && !scrubbing_ && timeline_.playhead != lastSentFrame_) {
        syncSession_.sendSeek(timeline_.playhead);
        lastSentFrame_ = timeline_.playhead;
    }
    // Zoom/pan: emit when it changes, and re-emit whenever a new spectator joins
    // (zoom/pan isn't carried in the project snapshot, so a late joiner needs it).
    int peers = syncSession_.peerCount();
    if (peers > lastPeerCount_) {
        lastSentZoom_ = -1.0f;        // force a re-send below
        lastSentExpGain_ = 999.0f;    // ditto, out of the gain's range
        lastSentMatteRatio_ = -1.0f;  // ditto, below every real ratio
    }
    lastPeerCount_ = peers;
    // The matte, ahead of the view: Fit View decides which rectangle the zoom/pan
    // below is relative to, so a spectator has to have it before decoding them.
    if (timeline_.letterboxRatio != lastSentMatteRatio_ ||
        timeline_.letterboxOpacity != lastSentMatteOpacity_ ||
        letterboxFitView_ != lastSentMatteFit_) {
        syncSession_.sendMatte((float)timeline_.letterboxRatio, timeline_.letterboxOpacity,
                               letterboxFitView_);
        lastSentMatteRatio_ = (float)timeline_.letterboxRatio;
        lastSentMatteOpacity_ = timeline_.letterboxOpacity;
        lastSentMatteFit_ = letterboxFitView_;
    }
    float zoom = 1.0f, u = 0.5f, v = 0.5f;
    frameViewCenter(zoom, u, v);
    if (zoom != lastSentZoom_ || u != lastSentU_ || v != lastSentV_) {
        syncSession_.sendView(zoom, u, v);
        lastSentZoom_ = zoom;
        lastSentU_ = u;
        lastSentV_ = v;
    }
    // Exposure/gamma: same deal — emit on change, and to every new joiner (it
    // isn't in the project snapshot either).
    if (grade_.gain != lastSentExpGain_ || grade_.gamma != lastSentExpGamma_) {
        syncSession_.sendExposure(grade_.gain, grade_.gamma);
        lastSentExpGain_ = grade_.gain;
        lastSentExpGamma_ = grade_.gamma;
    }
}

// Host: serialize the whole frame's stroke set and broadcast it as the combined
// annotations (called on the host's own release and whenever a spectator stroke
// is merged). Encoding must match the Annotations decoder in drainSync().
void App::broadcastAnnotations() {
    if (syncSession_.role() != syncreview::Role::Host)
        return;
    std::string b;
    auto put = [&](const void* p, size_t n) { b.append((const char*)p, n); };
    int64_t frame = timeline_.playhead;
    uint32_t count = (uint32_t)annotStrokes_.size();
    put(&frame, 8);
    put(&count, 4);
    for (const auto& s : annotStrokes_) {
        uint32_t n = (uint32_t)s.pts.size();
        put(&s.r, 4);
        put(&s.g, 4);
        put(&s.b, 4);
        put(&n, 4);
        for (const auto& p : s.pts) {
            put(&p.x, 4);
            put(&p.y, 4);
            put(&p.hw, 4);
        }
    }
    syncSession_.broadcastAnnotations(b);
}

// Spectator: serialize the just-completed stroke (back of annotStrokes_) and send
// it to the host, which merges and re-broadcasts. Encoding must match the Stroke
// decoder in drainSync(). Called on mouse release.
void App::sendStrokeToHost() {
    if (syncSession_.role() != syncreview::Role::Spectator || annotStrokes_.empty())
        return;
    const AnnotStroke& s = annotStrokes_.back();
    std::string b;
    auto put = [&](const void* p, size_t n) { b.append((const char*)p, n); };
    int64_t frame = timeline_.playhead;
    uint32_t n = (uint32_t)s.pts.size();
    put(&frame, 8);
    put(&s.r, 4);
    put(&s.g, 4);
    put(&s.b, 4);
    put(&n, 4);
    for (const auto& p : s.pts) {
        put(&p.x, 4);
        put(&p.y, 4);
        put(&p.hw, 4);
    }
    syncSession_.sendStrokeToHost(b);
}

// Clear the frame's markup across the session. The host broadcasts the clear to
// every viewer; a spectator asks the host, which relays it (see drainSync).
void App::broadcastClearStrokes() {
    if (syncSession_.role() == syncreview::Role::Host)
        syncSession_.sendClear(timeline_.playhead);
    else if (syncSession_.role() == syncreview::Role::Spectator)
        syncSession_.sendClearToHost(timeline_.playhead);
}

void App::refreshHostSnapshot() {
    if (syncSession_.role() != syncreview::Role::Host) {
        return;
    }
    int clipCount = 0;
    timeline_.forEachClip([&](const Clip&) { ++clipCount; });
    std::string buf, err;
    if (Project::saveBuffer(buf, timeline_, projectId_, err, viewState())) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[sync] refreshHostSnapshot: broadcasting %zu bytes "
                    "(media:%zu seqs:%zu clips:%d viewSeq:%d peers:%d)",
                    buf.size(), timeline_.media.size(), timeline_.sequences.size(),
                    clipCount, viewSeqIdx_, syncSession_.peerCount());
        syncSession_.broadcastProject(buf);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[sync] refreshHostSnapshot: saveBuffer FAILED: %s", err.c_str());
    }
}

// Spectator: discard whatever is loaded and adopt the host's project (received
// over TCP). Mirrors loadProject() but from a byte buffer and with no on-disk
// association — the received copy is transient.
void App::loadProjectFromBuffer(const std::string& bytes) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[sync] loadProjectFromBuffer: received %zu bytes", bytes.size());
    work_.reset();
    joinRefresh();

    Timeline tl;
    int nc = 1, ns = 1, nsh = 1;
    std::string id, err;
    Project::ViewState savedView;
    if (!Project::loadBuffer(bytes, tl, nc, ns, nsh, id, err, &savedView)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[sync] loadProjectFromBuffer: loadBuffer FAILED: %s", err.c_str());
        setStatus("SESSION PROJECT LOAD FAILED: " + err, 5000);
        return;
    }
    int clipCount = 0;
    tl.forEachClip([&](const Clip&) { ++clipCount; });
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[sync] loadProjectFromBuffer: parsed OK "
                "(media:%zu seqs:%zu clips:%d shots:%zu savedViewSeq:%d savedViewProj:%d)",
                tl.media.size(), tl.sequences.size(), clipCount, tl.shots.size(),
                savedView.seqIdx, savedView.projId);
    timeline_ = std::move(tl);
    undoStack_.clear();
    nextClipId_ = nc;
    nextSeqId_ = ns;
    nextShotId_ = nsh;
    adoptLoadedTransitions();
    projectId_ = id;
    projectPath_ = "session.jpproj";
    projectHasPath_ = false; // received copy; Save prompts for a new location
    projectFromShared_ = false;
    cache_->clear();
    clearFramePreview();
    playing_ = false;
    hasTexture_ = false;
    displayedKey_ = CacheKey{};
    hideLauncher_ = true; // show the timeline, not the recent-projects launcher
    applyViewState(savedView); // mirror the host's scope; no saved one → first sequence
    relocateRules_.clear();
    relocateRuleTried_.clear();
    loadOrigin_ = LoadOrigin::Project;
    loadSourceLabel_ = "session";
    finishLoad();
    setStatus("JOINED SESSION — PROJECT LOADED");
}

// ── SESSION panel ─────────────────────────────────────────────────────────

void App::renderSessionPanel() {
    sessionBeaconRows_.clear();
    sessionNetChkRect_ = sessionCreateRect_ = sessionJoinManualRect_ = sessionLeaveRect_ = SDL_FRect{};
    if (!panelOpen(kPanelSync))
        return;

    // Left-side pane, mutually exclusive with the other left panels: flush against
    // the icon strip, running from under the title bar down to the timeline.
    const float topH = titleBar_.height();
    const float panelX = kSidePanelW;
    const float pY = topH;
    const float pH = panelsBottom_ - topH;
    SDL_FRect panel = { panelX, pY, openPanelW(), pH };
    setColor(renderer_, kBg);
    jplay::fillRect(renderer_, &panel);
    setColor(renderer_, SDL_Color{ 60, 64, 74, 255 }); // right-edge border (matches other panels)
    jplay::drawLine(renderer_, panelX + openPanelW() - 0.5f, pY, panelX + openPanelW() - 0.5f, panelsBottom_);

    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);

    const float pad = 10.0f * dpiScale;
    const float lh = textFont_.lineHeight();
    const float rowH = 24.0f * dpiScale;
    SDL_FRect body = inset(panel, pad, 0.0f);
    gapTop(body, pad);

    // Everything except the master switch is inert while networking is off. It
    // still draws — dimmed — so the panel keeps its shape when the box is ticked,
    // and the helpers below simply don't publish hit rects for it.
    const bool net = syncNetwork_;
    auto dim = [net](SDL_Color c) -> SDL_Color {
        if (net)
            return c;
        return { (Uint8)(c.r * 45 / 100), (Uint8)(c.g * 45 / 100), (Uint8)(c.b * 45 / 100), c.a };
    };

    // Text row helper: one line of the panel's full width.
    auto line = [&](SDL_Color col, const std::string& s) {
        SDL_FRect r = cutTop(body, lh);
        drawText(r.x, r.y, dim(col), s.c_str());
    };

    // Button helper: a full-width row cut off the top of the body. Disabled, it
    // draws flat and reports no hit rect, so the click handler can't reach it.
    auto button = [&](SDL_FRect& r, const char* label, SDL_Color face) {
        const SDL_FRect row = cutTop(body, rowH);
        const bool hov = net && inRect(row, mx, my);
        drawButton(renderer_, &textFont_, row, label, hov ? kBtnHi : dim(face), dim(kBtnEdge),
                   dim(kValue));
        r = net ? row : SDL_FRect{};
    };

    // Labelled text field: caption row, then the field row.
    auto field = [&](TextInput& fld, const char* caption) {
        line(kLabel, caption);
        gapTop(body, 3.0f);
        fld.setRect(cutTop(body, rowH));
        gapTop(body, 8.0f * dpiScale);
        setColor(renderer_, dim(kFieldBg));
        jplay::fillRect(renderer_, &fld.rect());
        fld.render(renderer_, &textFont_);
    };

    // ── network master switch ──
    // Drawn before the role branches below, so it stays reachable while hosting
    // or watching: unticking it leaves the session (see toggleSyncNetwork).
    {
        SDL_FRect head = cutTop(body, lh);
        drawText(head.x, head.y, kHead, "NETWORK");
        gapTop(body, 6.0f * dpiScale);

        sessionNetChkRect_ = cutTop(body, rowH);
        SDL_FRect band = sessionNetChkRect_;
        const float boxSz = 14.0f * dpiScale;
        const SDL_FRect box = centerV(cutLeft(band, boxSz), boxSz);
        setColor(renderer_, net ? kAccent : kFieldBg);
        jplay::fillRect(renderer_, &box);
        setColor(renderer_, kBtnEdge);
        jplay::drawRect(renderer_, &box);
        if (net) {
            const SDL_FRect tick = inset(box, 3.0f, 3.0f);
            setColor(renderer_, kValue);
            jplay::fillRect(renderer_, &tick);
        }
        const SDL_FRect t = centerV(sessionNetChkRect_, lh);
        drawText(box.x + boxSz + 6.0f * dpiScale, t.y, net ? kValue : kLabel, "Enable Sync Review Socket");
        gapTop(body, 8.0f * dpiScale);
    }
    // Host port. Applied when a session is created; a spectator joining by beacon
    // gets the host's port from the beacon, a manual join types "ip:port".
    field(sessionPortFld_, "Port");

    setColor(renderer_, kEdge);
    jplay::drawLine(renderer_, body.x, body.y, body.x + body.w, body.y);
    gapTop(body, 8.0f * dpiScale);
    line(kHead, "CREATE SESSION");
    gapTop(body, 10.0f * dpiScale);

    const syncreview::Role role = syncSession_.role();

    if (role == syncreview::Role::Host) {
        line(kHostOn, "HOSTING");
        gapTop(body, 16.0f * dpiScale);
        button(sessionLeaveRect_, "End Session", kBtnBg);
        gapTop(body, 12.0f * dpiScale);

        auto peers = syncSession_.peerList();
        if (peers.empty()) {
            line(kLabel, "No viewers connected");
        } else {
            for (const auto& p : peers) {
                const std::string& host = p.hostname.empty() ? p.ip : p.hostname;
                line(kValue, fitText(p.name + "  —  " + host, body.w));
                gapTop(body, 4.0f);
            }
        }
        return;
    }
    if (role == syncreview::Role::Spectator) {
        line(kHostOn, "WATCHING");
        gapTop(body, 4.0f);
        line(kLabel, "Following host — transport locked");
        gapTop(body, 12.0f * dpiScale);
        button(sessionLeaveRect_, "Leave Session", kBtnBg);
        return;
    }

    // ── role == None: create / join UI ──
    field(sessionUserFld_, "Your name");
    button(sessionCreateRect_, "Create Session", kBtnBg);
    gapTop(body, 16.0f * dpiScale);

    setColor(renderer_, kEdge);
    jplay::drawLine(renderer_, body.x, body.y, body.x + body.w, body.y);
    gapTop(body, 8.0f * dpiScale);
    line(kHead, "AVAILABLE SESSIONS");
    gapTop(body, 6.0f);

    auto beacons = syncSession_.beacons();
    if (beacons.empty()) {
        line(kLabel, net ? "Searching…" : "Networking disabled");
        gapTop(body, 6.0f);
    }
    for (const auto& b : beacons) {
        SDL_FRect r = cutTop(body, rowH);
        gapTop(body, 4.0f);
        bool hov = inRect(r, mx, my);
        setColor(renderer_, hov ? kBtnHi : kBtnBg);
        jplay::fillRect(renderer_, &r);
        std::string name = b.username.empty() ? b.hostname : b.username;
        SDL_FRect text = r;
        gapLeft(text, 6.0f);
        const SDL_FRect t = centerV(text, lh);
        drawText(t.x, t.y, kValue, fitText(name + "  (" + b.ip + ")", r.w - 12.0f).c_str());
        sessionBeaconRows_.push_back({ b.ip, b.tcpPort, r });
    }

    gapTop(body, 8.0f * dpiScale);
    field(sessionHostFld_, "Host IP (manual)");
    button(sessionJoinManualRect_, "Join", kBtnBg);
}

bool App::sessionHandleEvent(const SDL_Event& e) {
    if (!panelOpen(kPanelSync))
        return false;

    // While a field is focused, route typing to it and consume the event so it
    // never reaches the global keyboard shortcuts.
    bool typing = sessionUserFld_.focused() || sessionHostFld_.focused() ||
                  sessionPortFld_.focused();
    if (typing && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT)) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_RETURN) {
            if (sessionHostFld_.focused())
                startJoinFromField();
            else if (sessionPortFld_.focused())
                commitSyncPort();
            return true;
        }
        sessionUserFld_.handleEvent(e);
        sessionHostFld_.handleEvent(e);
        sessionPortFld_.handleEvent(e);
        return true;
    }

    if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
        return false;
    float mx = e.button.x, my = e.button.y;

    // Clicks outside the panel are not ours. The pane occupies the left strip
    // [kSidePanelW, kSidePanelW + openPanelW()] from the title bar to the timeline;
    // the icon strip (session toggle) and the timeline handle their own clicks.
    if (mx < kSidePanelW || mx >= kSidePanelW + openPanelW()
        || my < titleBar_.height() || my >= panelsBottom_)
        return false;

    // The master switch is the one control that stays live with networking off.
    if (inRect(sessionNetChkRect_, mx, my)) {
        toggleSyncNetwork();
        return true;
    }
    if (!syncNetwork_)
        return true; // rest of the panel is disabled; swallow the click

    const syncreview::Role role = syncSession_.role();

    if (role != syncreview::Role::None) {
        if (inRect(sessionLeaveRect_, mx, my)) {
            leaveSession();
            setStatus("LEFT SESSION");
        }
        return true;
    }

    // role == None: focus fields / trigger actions. A click that takes focus off
    // the port field is what commits the typed value.
    const bool portWasFocused = sessionPortFld_.focused();
    sessionUserFld_.handleEvent(e);
    sessionHostFld_.handleEvent(e);
    sessionPortFld_.handleEvent(e);
    if (portWasFocused && !sessionPortFld_.focused())
        commitSyncPort();
    if (sessionUserFld_.focused() || sessionHostFld_.focused() || sessionPortFld_.focused())
        SDL_StartTextInput(window_);

    if (inRect(sessionCreateRect_, mx, my)) {
        std::string user = sessionUserFld_.text().empty() ? "guest" : sessionUserFld_.text();
        std::string err;
        if (syncSession_.startHost(user, (uint16_t)syncPort_, err)) {
            refreshHostSnapshot();
            // Seed broadcast tracking so we don't re-emit the current state (the
            // project already carries the playhead a joiner will land on).
            lastSentPlaying_ = playing_;
            lastSentFrame_ = timeline_.playhead;
            lastSentSeq_ = (int32_t)viewSeqIdx_;
            frameViewCenter(lastSentZoom_, lastSentU_, lastSentV_);
            lastSentExpGain_ = grade_.gain;
            lastSentExpGamma_ = grade_.gamma;
            lastSentMatteRatio_ = (float)timeline_.letterboxRatio;
            lastSentMatteOpacity_ = timeline_.letterboxOpacity;
            lastSentMatteFit_ = letterboxFitView_;
            lastPeerCount_ = 0;
            hostSnapshotDirty_ = false; // just sent the full project above
            setStatus("HOSTING SESSION");
        } else {
            setStatus("HOST FAILED: " + err, 5000);
        }
        return true;
    }

    for (const auto& row : sessionBeaconRows_)
        if (inRect(row.rect, mx, my)) {
            joinHost(row.ip, row.port);
            return true;
        }

    if (inRect(sessionJoinManualRect_, mx, my)) {
        startJoinFromField();
        return true;
    }
    return true; // swallow other clicks inside the panel
}

// End the session and reset the host broadcast tracking, so a later session
// starts from a clean slate rather than diffing against the old one.
void App::leaveSession() {
    syncSession_.leave();
    lastSentSeq_ = -2;
    lastSentFrame_ = -1;
    lastSentZoom_ = -1.0f;
    lastPeerCount_ = 0;
}

// Flip the network master switch. Turning it off closes everything immediately:
// any session is left, the discovery listener is shut down, and the panel's
// fields lose focus so typing can't land in the now-disabled controls.
void App::toggleSyncNetwork() {
    syncNetwork_ = !syncNetwork_;
    if (!syncNetwork_) {
        if (syncSession_.role() != syncreview::Role::None) {
            leaveSession();
            setStatus("LEFT SESSION — NETWORKING DISABLED", 4000);
        }
        syncSession_.stopDiscovery();
        sessionUserFld_.setFocus(false);
        sessionHostFld_.setFocus(false);
        sessionPortFld_.setFocus(false);
        SDL_StopTextInput(window_);
    }
    writePrefs();
}

// Adopt the typed host port. Rejected input snaps the field back to the value in
// force, so what the panel shows is always what a Create Session would bind.
void App::commitSyncPort() {
    const long n = std::strtol(sessionPortFld_.text().c_str(), nullptr, 10);
    if (n <= 0 || n > 65535) {
        sessionPortFld_.setText(std::to_string(syncPort_));
        setStatus("PORT MUST BE 1-65535", 3000);
        return;
    }
    sessionPortFld_.setText(std::to_string(n)); // normalize "0045778" / stray text
    if ((int)n == syncPort_)
        return;
    syncPort_ = (int)n;
    writePrefs();
}

// Parse the manual "Host IP" field as "ip" or "ip:port" and connect.
void App::startJoinFromField() {
    std::string s = sessionHostFld_.text();
    if (s.empty()) {
        setStatus("ENTER A HOST IP", 3000);
        return;
    }
    uint16_t port = syncreview::kDefaultPort;
    std::string ip = s;
    if (auto c = s.find(':'); c != std::string::npos) {
        ip = s.substr(0, c);
        port = (uint16_t)std::strtoul(s.c_str() + c + 1, nullptr, 10);
    }
    joinHost(ip, port);
}

// Joining adopts the host's project wholesale (see loadProjectFromBuffer), so it
// discards the local one — gated here, the single choke point for all three join
// entries (launcher column, session panel row, manual host field).
void App::joinHost(const std::string& ip, uint16_t port) {
    confirmDiscard("Join Session", [this, ip, port] {
        std::string user = sessionUserFld_.text().empty() ? "guest" : sessionUserFld_.text();
        std::string err;
        if (syncSession_.join(ip, port, user, err)) {
            SDL_StopTextInput(window_);
            setStatus("CONNECTING TO " + ip + "…");
        } else {
            setStatus("JOIN FAILED: " + err, 5000);
        }
    });
}
