
import { useEffect, useMemo, useState } from "react";
import {
  Activity,
  Clock3,
  Settings2,
  Wifi,
  WifiOff,
  RotateCcw,
  TrafficCone,
  ShieldCheck,
} from "lucide-react";
import "./index.css";
import useTrafficLight from "./hooks/useTrafficLight";
import { onAuthStateChanged, signOut } from "firebase/auth";
import { auth, db } from "./firebase/config";
import { onValue, ref, update, set } from "firebase/database";
import Login from "./components/Login";

const initialTimings = {
  1: { green: 15, yellow: 3 },
  2: { green: 15, yellow: 3 },
  3: { green: 15, yellow: 3 },
};

function TrafficSignal({ state }) {
  return (
    <div className="traffic-signal">
      {["red", "yellow", "green"].map((light) => (
        <div
          key={light}
          className={`signal-light ${light} ${
            state === light ? "active" : ""
          }`}
        />
      ))}
    </div>
  );
}

function App() {
  // 1. Firebase connection
  const [user, setUser] = useState(null);
  const [authLoading, setAuthLoading] = useState(true);

  const { trafficConfig, loading, error } = useTrafficLight(Boolean(user));
  const [showLogin, setShowLogin] = useState(false);
  const [savingLane, setSavingLane] = useState(null);
  const [saveMessage, setSaveMessage] = useState("");
  // 2. Local states
  // These are command-selection states. Actual monitoring comes from ESP32 /status.
  const [mode, setMode] = useState("auto");
  const [deviceStatus, setDeviceStatus] = useState(null);
  const [heartbeatNow, setHeartbeatNow] = useState(Date.now());
  const [congestionStatus, setCongestionStatus] = useState({
    1: null,
    2: null,
    3: null,
  });
  const [adaptiveConfig, setAdaptiveConfig] = useState(null);
  const [adaptiveLoading, setAdaptiveLoading] = useState(false);
  const [adaptiveError, setAdaptiveError] = useState("");
  const [adaptiveInputs, setAdaptiveInputs] = useState({});
  const [adaptiveDirty, setAdaptiveDirty] = useState({});
  const [savingAdaptiveLane, setSavingAdaptiveLane] = useState(null);
  const [adaptiveSaveMessage, setAdaptiveSaveMessage] = useState("");
  const [statusError, setStatusError] = useState("");
  const [timings, setTimings] = useState(initialTimings);
  const [manualCommand, setManualCommand] = useState("none");
  const [controlMessage, setControlMessage] = useState("");
  const [sendingCommand, setSendingCommand] = useState(false);

    // Emergency vehicle monitoring
  const [emergencyData, setEmergencyData] = useState(null);
  const [emergencyLoading, setEmergencyLoading] = useState(false);
  const [emergencyError, setEmergencyError] = useState("");
  const [emergencyMessage, setEmergencyMessage] = useState("");
  const [emergencyActionLoading, setEmergencyActionLoading] = useState(false);
  const [lane1Radius, setLane1Radius] = useState("50");
  // 3. Synchronize Firebase timings
  
// Listen to Firebase emergency data
useEffect(() => {
  if (!user) {
    setEmergencyData(null);
    setEmergencyLoading(false);
    setEmergencyError("");
    return;
  }

  setEmergencyLoading(true);
  setEmergencyError("");

  const emergencyReference = ref(db, "emergency");

  const unsubscribe = onValue(
    emergencyReference,
    (snapshot) => {
      if (snapshot.exists()) {
        const data = snapshot.val();

        console.log("Emergency data received:", data);

        setEmergencyData(data);

        // Synchronize the detection radius from Firebase
        setLane1Radius(
          String(data.zones?.Lane_1?.radius ?? 50)
        );

        setEmergencyError("");
      } else {
        console.warn("No /emergency node found in Firebase.");

        setEmergencyData(null);
        setEmergencyError("");
      }

      setEmergencyLoading(false);
    },
    (firebaseError) => {
      console.error(
        "Emergency Firebase listener error:",
        firebaseError
      );

      setEmergencyError(
        firebaseError.message ||
        "Unable to read emergency data."
      );

      setEmergencyLoading(false);
    }
  );

  return () => unsubscribe();
}, [user]);


  useEffect(() => {
    if (!trafficConfig) return;

    setTimings((previous) => ({
      1: {
        ...previous[1],
        green: Number(trafficConfig.Lane_1?.green?.time ?? previous[1].green),
        yellow: Number(trafficConfig.Lane_1?.yellow?.time ?? previous[1].yellow),
      },
      2: {
        ...previous[2],
        green: Number(trafficConfig.Lane_2?.green?.time ?? previous[2].green),
        yellow: Number(trafficConfig.Lane_2?.yellow?.time ?? previous[2].yellow),
      },
      3: {
        ...previous[3],
        green: Number(trafficConfig.Lane_3?.green?.time ?? previous[3].green),
        yellow: Number(trafficConfig.Lane_3?.yellow?.time ?? previous[3].yellow),
      },
    }));
  }, [trafficConfig]);

  useEffect(() => {
    const unsubscribe = onAuthStateChanged(auth, (currentUser) => {
      setUser(currentUser);
      setAuthLoading(false);
    });

    return () => unsubscribe();
  }, []);

  // Close the sign-in dialog automatically after successful authentication.
  useEffect(() => {
    if (user) setShowLogin(false);
  }, [user]);

  // Public monitoring: everyone can read the ESP32 status feed.
  useEffect(() => {
    const statusReference = ref(db, "status");
    
    const unsubscribe = onValue(
      statusReference,
      (snapshot) => {
        const data = snapshot.exists() ? snapshot.val() : null;

        setDeviceStatus(data);
        setStatusError("");

      },
      (firebaseError) => {
        console.error("ESP32 status listener error:", firebaseError);
        setStatusError(
          firebaseError.message || "Unable to read ESP32 status."
        );
      }
    );

    return () => unsubscribe();
  }, []);

    // Public monitoring: listen to congestion status for each lane.
    useEffect(() => {
      console.log("Starting Firebase congestion listeners...");
      const unsubscribers = [1, 2, 3].map((lane) => {
        const congestionReference = ref(
          db,
          `adaptiveTraffic/Lane_${lane}/congested`
        );

        return onValue(
          congestionReference,
          (snapshot) => {
            const value = snapshot.exists() ? Boolean(snapshot.val()) : null;

            console.log(`Lane ${lane} congestion received:`, value);

            setCongestionStatus((previous) => ({
              ...previous,
              [lane]: value,
            }));
          },
          (firebaseError) => {
            console.error(
              `Lane ${lane} congestion listener error:`,
              firebaseError
            );
          }
        );
      });

      return () => {
        unsubscribers.forEach((unsubscribe) => unsubscribe());
      };
    }, []);

    // Admin-only: read adaptive traffic configuration.
useEffect(() => {
  if (!user) {
    setAdaptiveConfig(null);
    setAdaptiveLoading(false);
    setAdaptiveError("");
    return;
  }

  setAdaptiveLoading(true);
  setAdaptiveError("");

  const adaptiveReference = ref(db, "adaptiveTraffic");

    const unsubscribe = onValue(
      adaptiveReference,
      (snapshot) => {
        if (snapshot.exists()) {
          const data = snapshot.val();

          console.log("Adaptive configuration received:", data);

          setAdaptiveConfig(data);
          setAdaptiveError("");
        } else {
          setAdaptiveConfig(null);
          setAdaptiveError("No adaptive configuration found.");
        }

        setAdaptiveLoading(false);
      },
      (firebaseError) => {
        console.error("Adaptive configuration read error:", firebaseError);

        setAdaptiveError(firebaseError.message);
        setAdaptiveLoading(false);
      }
    );

    return () => unsubscribe();
  }, [user]);

  // Synchronize editable adaptive settings from Firebase.
useEffect(() => {
  if (!adaptiveConfig) return;

  setAdaptiveInputs((previous) => {
    const next = { ...previous };

    [1, 2, 3].forEach((lane) => {
      // Don't overwrite inputs that the admin is currently editing.
      if (adaptiveDirty[lane]) return;

      const laneConfig = adaptiveConfig[`Lane_${lane}`];

      if (!laneConfig) return;

      next[lane] = {
        maxGreenDuration: String(
          laneConfig.maxGreenDuration ?? 60
        ),
        incrementSeconds: String(
          laneConfig.incrementSeconds ?? 5
        ),
      };
    });

    return next;
  });
}, [adaptiveConfig, adaptiveDirty]);

    useEffect(() => {
    const timer = setInterval(() => {
      setHeartbeatNow(Date.now());
    }, 1000);

    return () => clearInterval(timer);
  }, []);

  const lastUpdated = Number(deviceStatus?.lastUpdated);

  const hasHeartbeat =
    Number.isFinite(lastUpdated) && lastUpdated > 0;

  const heartbeatAge = hasHeartbeat
    ? heartbeatNow - lastUpdated
    : Infinity;

  let connectionStatus = "offline";

  // Allow up to 60 seconds of browser/Firebase clock difference.
  if (hasHeartbeat && heartbeatAge >= -60000 && heartbeatAge <= 10000) {
    connectionStatus = "online";
  } else if (hasHeartbeat && heartbeatAge > 10000 && heartbeatAge <= 20000) {
    connectionStatus = "reconnecting";
  }

  const connectionLabel = {
    online: "ESP32 ONLINE",
    reconnecting: "ESP32 RECONNECTING",
    offline: "ESP32 NOT ONLINE",
  }[connectionStatus];

  const displayMode = deviceStatus?.mode || mode;

  // Prefer explicit lane states sent by the ESP32. If the firmware only sends
  // phase/currentLane/activeLanes, derive the display from those fields.
  const laneStates = useMemo(() => {
    const states = { 1: "red", 2: "red", 3: "red" };

    if (!deviceStatus) return states;

    const reportedStates = deviceStatus.laneStates;
    if (reportedStates) {
      [1, 2, 3].forEach((lane) => {
        const value = String(reportedStates[lane] ?? reportedStates[`Lane_${lane}`] ?? "red").toLowerCase();
        states[lane] = ["red", "yellow", "green"].includes(value) ? value : "red";
      });
      return states;
    }

    const phase = String(deviceStatus.phase || "").toLowerCase();
    const activeLanes = Array.isArray(deviceStatus.activeLanes)
      ? deviceStatus.activeLanes.map(Number)
      : [];

    if (phase.includes("yellow") || phase.includes("transition")) {
      activeLanes.forEach((lane) => {
        if ([1, 2, 3].includes(lane)) states[lane] = "yellow";
      });

      // Supports phase names such as lane_1_lane_2_yellow if activeLanes
      // has not been added to the ESP32 status yet.
      if (activeLanes.length === 0) {
        [1, 2, 3].forEach((lane) => {
          if (phase.includes(`lane_${lane}`)) states[lane] = "yellow";
        });
      }
    } else if (phase.includes("green") || phase.startsWith("lane_")) {
      const lane = Number(deviceStatus.currentLane) ||
        Number(phase.match(/lane[_ ]?(1|2|3)/)?.[1]);

      if ([1, 2, 3].includes(lane)) {
        states[lane] = "green";
      } else if (activeLanes.length > 0) {
        activeLanes.forEach((activeLane) => {
          if ([1, 2, 3].includes(activeLane)) states[activeLane] = "green";
        });
      }
    } else if (phase === "all_yellow") {
      states[1] = states[2] = states[3] = "yellow";
    }

    return states;
  }, [deviceStatus]);

  const activeLane = (() => {
    if (!deviceStatus) return "Waiting for ESP32";

    if ([1, 2, 3].some((lane) => laneStates[lane] === "yellow")) return "Transition";

    const currentLane = Number(deviceStatus.currentLane);
    if (currentLane >= 1 && currentLane <= 3) return `Lane ${currentLane}`;

    const greenLanes = [1, 2, 3].filter((lane) => laneStates[lane] === "green");
    if (greenLanes.length === 1) return `Lane ${greenLanes[0]}`;
    if (greenLanes.length > 1) return "Multiple lanes";

    return "None";
  })();

  const remainingSeconds = Number(deviceStatus?.remainingSeconds);
  const hasRemainingTime =
    deviceStatus?.remainingSeconds !== undefined &&
    deviceStatus?.remainingSeconds !== null &&
    Number.isFinite(remainingSeconds);

  function updateTiming(lane, value) {
    const safeValue = Math.max(1, Math.min(300, Number(value) || 1));

    setTimings((previous) => ({
      ...previous,
      [lane]: {
        ...previous[lane],
        green: safeValue,
      },
    }));
  }

async function saveTiming(lane) {
  if (!user) {
    setSaveMessage("Please sign in first.");
    return;
  }

  const newMinimum = Number(timings[lane].green);

  const maximum = Number(
    adaptiveConfig?.[`Lane_${lane}`]?.maxGreenDuration ?? 60
  );

  if (!Number.isInteger(newMinimum) || newMinimum <= 0) {
    setSaveMessage(
      `Lane ${lane}: Minimum green duration must be a positive whole number.`
    );
    return;
  }

  if (newMinimum > maximum) {
    setSaveMessage(
      `Lane ${lane}: Minimum cannot exceed the adaptive maximum of ${maximum}s. Increase the maximum first.`
    );
    return;
  }

  try {
    setSavingLane(lane);
    setSaveMessage("");

    await update(
      ref(db, `trafficLight/Lane_${lane}/green`),
      {
        time: newMinimum,
      }
    );

    setSaveMessage(
      `Lane ${lane}: Minimum green duration saved successfully!`
    );
  } catch (err) {
    console.error("Firebase save error:", err);

    setSaveMessage(
      `Failed to save Lane ${lane}: ${err.message}`
    );
  } finally {
    setSavingLane(null);
  }
}



function updateAdaptiveInput(lane, field, value) {
  setAdaptiveInputs((previous) => ({
    ...previous,
    [lane]: {
      ...previous[lane],
      [field]: value,
    },
  }));

  setAdaptiveDirty((previous) => ({
    ...previous,
    [lane]: true,
  }));
}

async function saveAdaptiveSettings(lane) {
  if (!user || savingAdaptiveLane !== null) return;

  const maxGreenDuration = Number(
    adaptiveInputs[lane]?.maxGreenDuration
  );

  const incrementSeconds = Number(
    adaptiveInputs[lane]?.incrementSeconds
  );

  const originalGreenDuration = Number(
    adaptiveConfig?.[`Lane_${lane}`]?.originalGreenDuration
  );

  if (
    !Number.isInteger(maxGreenDuration) ||
    !Number.isInteger(incrementSeconds) ||
    !Number.isFinite(originalGreenDuration) ||
    maxGreenDuration < originalGreenDuration ||
    incrementSeconds <= 0
  ) {
    setAdaptiveSaveMessage(
      `Lane ${lane}: Maximum must be at least ${originalGreenDuration}s and increment must be a positive whole number.`
    );
    return;
  }

  try {
    setSavingAdaptiveLane(lane);
    setAdaptiveSaveMessage("");

    await Promise.all([
      set(
        ref(db, `adaptiveTraffic/Lane_${lane}/maxGreenDuration`),
        maxGreenDuration
      ),
      set(
        ref(db, `adaptiveTraffic/Lane_${lane}/incrementSeconds`),
        incrementSeconds
      ),
    ]);

    setAdaptiveSaveMessage(
      `Lane ${lane}: Adaptive settings saved successfully!`
    );
    setAdaptiveDirty((previous) => ({
  ...previous,
  [lane]: false,
}));
  } catch (err) {
    console.error("Adaptive settings save error:", err);

    setAdaptiveSaveMessage(
      `Lane ${lane}: Save failed — ${err.message}`
    );
  } finally {
    setSavingAdaptiveLane(null);
  }
}
  async function sendControlCommand(newMode, command) {
    if (!user || sendingCommand) return;

    try {
      setSendingCommand(true);
      setControlMessage("");

      await update(ref(db, "control"), {
        mode: newMode,
        manualCommand: command,
        commandId: Date.now(),
      });

      // Update the local dashboard after Firebase succeeds.
      setMode(newMode);
      setManualCommand(command);

      setControlMessage("Command sent successfully!");
    } catch (err) {
      console.error("Control command error:", err);
      setControlMessage(`Command failed: ${err.message}`);
    } finally {
      setSendingCommand(false);
    }
  }

  // Request ESP32 to update Lane 1 zone coordinates
async function requestLane1ZoneUpdate() {
  if (!user || emergencyActionLoading) return;

  try {
    setEmergencyActionLoading(true);
    setEmergencyMessage("");

    await update(
      ref(db, "emergency/zones/Lane_1"),
      {
        update: true,
      }
    );

    setEmergencyMessage(
      "Zone update requested. Waiting for the emergency ESP32..."
    );

  } catch (err) {
    console.error("Zone update request failed:", err);

    setEmergencyMessage(
      `Zone update failed: ${err.message}`
    );
  } finally {
    setEmergencyActionLoading(false);
  }
}


// Enable or disable Lane 1 emergency detection
async function toggleLane1Emergency(enabled) {
    if (!user || emergencyActionLoading) return;

    try {
      setEmergencyActionLoading(true);
      setEmergencyMessage("");

      await update(
        ref(db, "emergency/zones/Lane_1"),
        {
          enabled,
        }
      );

      setEmergencyMessage(
        `Lane 1 emergency detection ${enabled ? "enabled" : "disabled"}.`
      );

    } catch (err) {
      console.error("Lane 1 setting update failed:", err);

      setEmergencyMessage(
        `Update failed: ${err.message}`
      );
    } finally {
      setEmergencyActionLoading(false);
    }
  }


  // Save Lane 1 detection radius
  async function saveLane1Radius() {
    if (!user || emergencyActionLoading) return;

    const radius = Number(lane1Radius);

    if (!Number.isFinite(radius) || radius <= 0 || radius > 10000) {
      setEmergencyMessage("Enter a valid radius between 1 and 10000 metres.");
      return;
    }

    try {
      setEmergencyActionLoading(true);
      setEmergencyMessage("");

      await set(
        ref(db, "emergency/zones/Lane_1/radius"),
        radius
      );

      setEmergencyMessage(
        `Lane 1 detection radius updated to ${radius} metres.`
      );

    } catch (err) {
      console.error("Radius update failed:", err);

      setEmergencyMessage(
        `Radius update failed: ${err.message}`
      );
    } finally {
      setEmergencyActionLoading(false);
    }
  }

  function selectManualCommand(command) {
    sendControlCommand("manual", command);
  }

  function switchToAuto() {
    sendControlCommand("auto", "none");
  }

  function resetTimings() {
    setTimings(initialTimings);
  }

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-icon">
            <TrafficCone size={23} />
          </div>
          <div>
            <h2>Signal<span>Sync</span></h2>
            <p>TRAFFIC CONTROL</p>
          </div>
        </div>

        <div className="sidebar-label">WORKSPACE</div>

        <div className="nav-item selected">
          <Activity size={18} />
          <span>Dashboard</span>
        </div>

        <div className="nav-item">
          <Settings2 size={18} />
          <span>Configuration</span>
          <span className="coming-soon">SOON</span>
        </div>

        <div className="sidebar-bottom">
          <div className="device-mini">
            <div className="device-icon">
              {connectionStatus === "online" ? (
                <Wifi size={17} />
              ) : (
                <WifiOff size={17} />
              )}
            </div>

            <div>
              <strong>ESP32 Controller</strong>
              <small>
                {connectionStatus === "online"
                  ? "ESP32 online"
                  : connectionStatus === "reconnecting"
                  ? "Reconnecting..."
                  : "ESP32 offline"}
              </small>
            </div>

            <span
              className="online-dot"
              style={{
                opacity: connectionStatus === "online" ? 1 : 0.35,
                background:
                  connectionStatus === "reconnecting" ? "#eab308" : undefined,
              }}
            />
          </div>
          <p className="sidebar-version">SIGNALSYNC · VERSION 1.0</p>
        </div>
      </aside>

      <main className="main-content">
        <header className="topbar">
          <div>
            <div className="breadcrumb">WORKSPACE / DASHBOARD</div>
            <h1>Traffic Overview</h1>
            <p className="page-subtitle">
              Monitor and manage your three-lane traffic system.
            </p>
          </div>

          <div className="topbar-right">
            <div className="demo-badge">
              <span
                style={{
                  background:
                    connectionStatus === "online"
                      ? "#22c55e"
                      : connectionStatus === "reconnecting"
                      ? "#eab308"
                      : "#ef4444",
                }}
              />

              {connectionLabel}
            </div>

            {user ? (
              <>
                <div className="avatar">
                  {user.email?.charAt(0).toUpperCase() || "A"}
                </div>
                <button
                  onClick={() => signOut(auth)}
                  style={{
                    padding: "9px 14px",
                    border: "1px solid #444",
                    borderRadius: "8px",
                    background: "#222",
                    color: "#fff",
                    cursor: "pointer",
                    fontSize: "13px",
                  }}
                >
                  Sign Out
                </button>
              </>
            ) : (
              <button
                onClick={() => setShowLogin(true)}
                disabled={authLoading}
                style={{
                  padding: "9px 14px",
                  border: "1px solid #444",
                  borderRadius: "8px",
                  background: "#222",
                  color: "#fff",
                  cursor: authLoading ? "not-allowed" : "pointer",
                  fontSize: "13px",
                  opacity: authLoading ? 0.6 : 1,
                }}
              >
                Sign In
              </button>
            )}
          </div>
        </header>

        <section className="overview-grid">
          <div className="overview-card">
            <div className="overview-icon green-icon"><Activity size={19} /></div>
            <div>
              <p>ACTIVE LANE</p>
              <h3>{activeLane}</h3>
              <small>Current signal state</small>
            </div>
          </div>

          <div className="overview-card">
            <div className="overview-icon blue-icon"><Clock3 size={19} /></div>
            <div>
              <p>TIME REMAINING</p>
              <h3>{displayMode === "auto" && hasRemainingTime ? `${remainingSeconds}s` : "—"}</h3>
              <small>{displayMode === "auto" ? "Reported by ESP32" : "Manual control"}</small>
            </div>
          </div>

          <div className="overview-card">
            <div className="overview-icon purple-icon"><Settings2 size={19} /></div>
            <div>
              <p>OPERATING MODE</p>
              <h3>{displayMode === "auto" ? "Automatic" : "Manual"}</h3>
              <small>Controller operation</small>
            </div>
          </div>  
          <div className="overview-card">
            <div className="overview-icon orange-icon">
              {error ? <WifiOff size={19} /> : <Wifi size={19} />}
            </div>
            <div>
              <p>FIREBASE DATABASE</p>
              <h3>
                {loading ? "Connecting..." : error || statusError ? "Connection error" : "Connected"}
              </h3>
              <small>
                {error || statusError || (loading ? "Checking database..." : "Firebase listener active")}
              </small>
            </div>
          </div>
        </section>

        <section className="section-heading">
          <div>
            <h2>Lane Monitoring</h2>
            <p>Current signal indication for each lane.</p>
          </div>
          <span className="demo-label">{deviceStatus ? "LIVE DEVICE DATA" : "WAITING FOR STATUS"}</span>
        </section>

        {(connectionStatus !== "online") ? (
          <div
            className="info-note"
            style={{
              marginBottom: "16px",
              padding: "18px",
              opacity: 0.72,
              background: "rgba(120, 120, 120, 0.10)",
              border: "1px dashed rgba(150, 150, 150, 0.55)",
              borderRadius: "12px",
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              gap: "10px",
              textAlign: "center",
            }}
            role="status"
            aria-live="polite"
          >
            <WifiOff size={18} />
            <span>
              <strong>
                {connectionStatus === "reconnecting"
                  ? "ESP32 CONNECTION UNSTABLE"
                  : "ESP32 NOT ONLINE"}
              </strong>

              <br />

              {connectionStatus === "reconnecting"
                ? "The ESP32 heartbeat is delayed. Waiting for the next update."
                : deviceStatus
                ? "No recent heartbeat received from the ESP32."
                : "Waiting for the ESP32 to send its first status update."}
            </span>
          </div>
        ): null}

        <section className="lane-grid">
          {[1, 2, 3].map((lane) => (
            <article className="lane-card" key={lane}>
              <div className="lane-card-top">
                <div>
                  <span className="lane-number">0{lane}</span>
                  <h3>Lane {lane}</h3>
                </div>

                <div className="lane-status-stack">
                  <span className={`state-pill ${laneStates[lane]}`}>
                    {laneStates[lane].toUpperCase()}
                  </span>

                  <span
                    className={`congestion-pill ${
                      congestionStatus[lane] === true
                        ? "congested"
                        : congestionStatus[lane] === false
                        ? "clear"
                        : "unknown"
                    }`}
                  >
                    {congestionStatus[lane] === true
                      ? "CONGESTED"
                      : congestionStatus[lane] === false
                      ? "CLEAR"
                      : "LOADING"}
                  </span>
                </div>
              </div>

              <div className="signal-area">
                <TrafficSignal state={laneStates[lane]} />
              </div>
            </article>
          ))}
        </section>

        {user ? (
          <>
          <section className="control-grid">
            <article className="panel timing-panel">
            <div className="panel-heading">
              <div>
                <h2>Timing Configuration</h2>
                <p>Set the green-light duration for each lane.</p>
              </div>
              <button className="icon-button" onClick={resetTimings} title="Reset timings">
                <RotateCcw size={17} />
              </button>
            </div>

            {[1, 2, 3].map((lane) => (
              <div className="timing-row" key={lane}>
                <div className="timing-lane">
                  <span className="mini-lane-dot" />
                  <strong>Lane {lane}</strong>
                </div>

                <div className="timing-input-wrap">
                  <input
                    type="number"
                    min="1"
                    max="300"
                    value={timings[lane].green}
                    onChange={(event) => updateTiming(lane, event.target.value)}
                  />
                  <span>SEC</span>

                  <button
                    onClick={() => saveTiming(lane)}
                    disabled={savingLane !== null}
                    style={{
                      padding: "8px 12px",
                      marginLeft: "8px",
                      border: "none",
                      borderRadius: "6px",
                      background: "#16a34a",
                      color: "white",
                      cursor: savingLane !== null ? "not-allowed" : "pointer",
                      opacity: savingLane !== null ? 0.6 : 1,
                    }}
                  >
                    {savingLane === lane ? "Saving..." : "Save"}
                  </button>
                </div>
              </div>
            ))}

            <div className="info-note">
              <Clock3 size={16} />
              <span>
                {saveMessage || "Edit a lane duration and click Save to update Firebase."}
              </span>
            </div>
          </article>

          <article className="panel manual-panel">
            <div className="panel-heading">
              <div>
                <h2>Control Center</h2>
                <p>Choose how the traffic signals operate.</p>
              </div>
              <ShieldCheck size={21} className="panel-heading-icon" />
            </div>

            <div className="mode-selector">
              <button
                disabled={sendingCommand}
                className={displayMode === "auto" ? "mode-button active" : "mode-button"}
                onClick={switchToAuto}
              >
                Automatic
              </button>
              <button
                disabled={sendingCommand}
                className={displayMode === "manual" ? "mode-button active" : "mode-button"}
                onClick={() => selectManualCommand("all_red")}
              >
                Manual
              </button>
            </div>

            <p className="control-label">MANUAL COMMANDS</p>

            <div className="manual-buttons">
              <button
                className={`manual-button red-command ${manualCommand === "all_red" && mode === "manual" ? "chosen" : ""}`}
                onClick={() => selectManualCommand("all_red")}
              >
                <span className="command-dot red-dot" />
                All Red
              </button>

              <button
                className={`manual-button yellow-command ${manualCommand === "all_yellow" && mode === "manual" ? "chosen" : ""}`}
                onClick={() => selectManualCommand("all_yellow")}
              >
                <span className="command-dot yellow-dot" />
                All Yellow
              </button>
            </div>

            <p className="control-label lane-command-label">SELECT A GREEN LANE</p>

            <div className="lane-command-buttons">
              {[1, 2, 3].map((lane) => (
                <button
                  key={lane}
                  className={`lane-command ${manualCommand === `lane_${lane}` && mode === "manual" ? "chosen" : ""}`}
                  onClick={() => selectManualCommand(`lane_${lane}`)}
                >
                  Lane {lane}
                </button>
              ))}
            </div>

            <div className="manual-warning">
              {sendingCommand
                ? "Sending command to Firebase..."
                : controlMessage || "Commands are sent to Firebase. Lane monitoring uses the ESP32 status feed."}
            </div>
            </article>
          </section>    
          <section className="panel adaptive-panel">
            <div className="panel-heading">
              <div>
                <h2>Adaptive Traffic Configuration</h2>
                <p>
                  Configure how green-light durations adapt to congestion.
                </p>
              </div>
              <Activity size={21} className="panel-heading-icon" />
            </div>

            {adaptiveLoading ? (
              <div className="info-note">
                Loading adaptive configuration...
              </div>
            ) : adaptiveError ? (
              <div className="info-note">
                {adaptiveError}
              </div>
            ) : !adaptiveConfig ? (
              <div className="info-note">
                No adaptive configuration available.
              </div>
            ) : (
              <div className="adaptive-lane-grid">
                {[1, 2, 3].map((lane) => {
                  const laneConfig =
                    adaptiveConfig[`Lane_${lane}`];

                  if (!laneConfig) return null;

                  return (
                    <article className="adaptive-lane-card" key={lane}>
                      <div className="adaptive-lane-heading">
                        <h3>Lane {lane}</h3>

                        <span
                          className={`congestion-pill ${
                            laneConfig.congested
                              ? "congested"
                              : "clear"
                          }`}
                        >
                          {laneConfig.congested
                            ? "CONGESTED"
                            : "CLEAR"}
                        </span>
                      </div>

                      <div className="adaptive-stat-grid">
                        <div className="adaptive-stat">
                          <span>Minimum Green</span>
                          <strong>
                            {laneConfig.originalGreenDuration ?? "—"}s
                          </strong>
                        </div>

                        <div className="adaptive-stat">
                          <span>Current Adaptive</span>
                          <strong>
                            {laneConfig.greenDuration ?? "—"}s
                          </strong>
                        </div>

                        <div className="adaptive-stat">
                          <span>Last Decision</span>
                          <strong>
                            {laneConfig.lastDecision ?? "—"}
                          </strong>
                        </div>
                      </div>

                      <div className="adaptive-input-group">
                        <label htmlFor={`max-duration-${lane}`}>
                          Maximum Green Duration (seconds)
                        </label>

                        <input
                          id={`max-duration-${lane}`}
                          type="number"
                          min={laneConfig.originalGreenDuration ?? 1}
                          step="1"
                          value={
                            adaptiveInputs[lane]?.maxGreenDuration ?? ""
                          }
                          onChange={(event) =>
                            updateAdaptiveInput(
                              lane,
                              "maxGreenDuration",
                              event.target.value
                            )
                          }
                        />
                      </div>

                      <div className="adaptive-input-group">
                        <label htmlFor={`increment-${lane}`}>
                          Increment per Decision (seconds)
                        </label>

                        <input
                          id={`increment-${lane}`}
                          type="number"
                          min="1"
                          step="1"
                          value={
                            adaptiveInputs[lane]?.incrementSeconds ?? ""
                          }
                          onChange={(event) =>
                            updateAdaptiveInput(
                              lane,
                              "incrementSeconds",
                              event.target.value
                            )
                          }
                        />
                      </div>

                      <button
                        className="adaptive-save-button"
                        onClick={() => saveAdaptiveSettings(lane)}
                        disabled={
                          savingAdaptiveLane !== null ||
                          !adaptiveDirty[lane]
                        }
                      >
                        {savingAdaptiveLane === lane
                          ? "Saving..."
                          : "Save Adaptive Settings"}
                      </button>
                    </article>
                  );
                })}
              </div>
            )}

            {adaptiveSaveMessage && (
              <div className="info-note">
                {adaptiveSaveMessage}
              </div>
            )}
          
{/* ================================================
    EMERGENCY VEHICLE MANAGEMENT
================================================ */}

<section className="panel adaptive-panel">
  <div className="panel-heading">
    <div>
      <h2>Emergency Vehicle Management</h2>
      <p>
        Monitor emergency vehicle location and configure
        priority zones.
      </p>
    </div>

    <ShieldCheck size={21} className="panel-heading-icon" />
  </div>

  {emergencyLoading ? (
    <div className="info-note">
      Loading emergency configuration...
    </div>
  ) : emergencyError ? (
    <div className="info-note">
      {emergencyError}
    </div>
  ) : !emergencyData ? (
    <div className="info-note">
      No emergency data available.
    </div>
  ) : (
    <>
      {/* Emergency status */}
      <article className="adaptive-lane-card">
        <div className="adaptive-lane-heading">
          <h3>Emergency Vehicle Status</h3>

          <span
            className={`congestion-pill ${
              emergencyData.emergency_vehicle_detected
                ? "congested"
                : "clear"
            }`}
          >
            {emergencyData.emergency_vehicle_detected
              ? "EMERGENCY DETECTED"
              : "NO EMERGENCY"}
          </span>
        </div>

        <div className="adaptive-stat-grid">
          <div className="adaptive-stat">
            <span>Priority Lane</span>
            <strong>
              {emergencyData.priority_lane || "None"}
            </strong>
          </div>

          <div className="adaptive-stat">
            <span>Location Source</span>
            <strong>
              {emergencyData.location_source || "Unknown"}
            </strong>
          </div>

          <div className="adaptive-stat">
            <span>Last Heartbeat</span>
            <strong>
              {Number(emergencyData.last_heartbeat) > 0
                ? new Date(
                    Number(emergencyData.last_heartbeat)
                  ).toLocaleTimeString()
                : "Unavailable"}
            </strong>
          </div>
        </div>

        <div className="adaptive-stat-grid">
          <div className="adaptive-stat">
            <span>Vehicle Latitude</span>
            <strong>
              {emergencyData.latitude ?? "—"}
            </strong>
          </div>

          <div className="adaptive-stat">
            <span>Vehicle Longitude</span>
            <strong>
              {emergencyData.longitude ?? "—"}
            </strong>
          </div>
        </div>
      </article>

      {/* Lane 1 zone settings */}
      <article
        className="adaptive-lane-card"
        style={{ marginTop: "18px" }}
      >
        <div className="adaptive-lane-heading">
          <h3>Lane 1 Emergency Zone</h3>

          <span
            className={`congestion-pill ${
              emergencyData.zones?.Lane_1?.enabled
                ? "clear"
                : "unknown"
            }`}
          >
            {emergencyData.zones?.Lane_1?.enabled
              ? "ENABLED"
              : "DISABLED"}
          </span>
        </div>

        <div className="adaptive-stat-grid">
          <div className="adaptive-stat">
            <span>Zone Latitude</span>
            <strong>
              {emergencyData.zones?.Lane_1?.latitude ?? "—"}
            </strong>
          </div>

          <div className="adaptive-stat">
            <span>Zone Longitude</span>
            <strong>
              {emergencyData.zones?.Lane_1?.longitude ?? "—"}
            </strong>
          </div>

          <div className="adaptive-stat">
            <span>Update Request</span>
            <strong>
              {emergencyData.zones?.Lane_1?.update
                ? "Pending"
                : "Inactive"}
            </strong>
          </div>
        </div>

        {/* Enable / Disable */}
        <div className="adaptive-input-group">
          <label>Emergency Zone Detection</label>

          <button
            className="adaptive-save-button"
            disabled={emergencyActionLoading}
            onClick={() =>
              toggleLane1Emergency(
                !emergencyData.zones?.Lane_1?.enabled
              )
            }
          >
            {emergencyData.zones?.Lane_1?.enabled
              ? "Disable Lane 1 Detection"
              : "Enable Lane 1 Detection"}
          </button>
        </div>

        {/* Detection radius */}
        <div className="adaptive-input-group">
          <label htmlFor="lane1-radius">
            Detection Radius (metres)
          </label>

          <input
            id="lane1-radius"
            type="number"
            min="1"
            max="10000"
            value={lane1Radius}
            onChange={(event) =>
              setLane1Radius(event.target.value)
            }
          />

          <button
            className="adaptive-save-button"
            style={{ marginTop: "10px" }}
            disabled={emergencyActionLoading}
            onClick={saveLane1Radius}
          >
            Save Detection Radius
          </button>
        </div>

        {/* Request zone update */}
        <div className="adaptive-input-group">
          <label>Update Emergency Zone Coordinates</label>

          <p style={{
            fontSize: "13px",
            opacity: 0.7,
            marginBottom: "10px"
          }}>
            Requests the ESP32 to copy the latest available
            GPS/Firebase coordinates into the Lane 1 zone.
          </p>

          <button
            className="adaptive-save-button"
            disabled={
              emergencyActionLoading ||
              !emergencyData.zones?.Lane_1?.enabled
            }
            onClick={requestLane1ZoneUpdate}
          >
            {emergencyActionLoading
              ? "Processing..."
              : emergencyData.zones?.Lane_1?.update
              ? "Zone Update Pending..."
              : "Update Lane 1 Zone"}
          </button>
        </div>
      </article>
    </>
  )}

  {emergencyMessage && (
    <div className="info-note" style={{ marginTop: "15px" }}>
      {emergencyMessage}
    </div>
  )}
</section>

          </section>
        </>
        ) : null}
        {showLogin && !user && (
          <div
            role="presentation"
            onClick={(event) => {
              if (event.target === event.currentTarget) setShowLogin(false);
            }}
            style={{
              position: "fixed",
              inset: 0,
              zIndex: 1000,
              background: "rgba(0, 0, 0, 0.68)",
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              padding: "20px",
              overflowY: "auto",
            }}
          >
            <div
              role="dialog"
              aria-modal="true"
              aria-label="Administrator sign in"
              style={{
                position: "relative",
                width: "min(100%, 460px)",
                maxHeight: "90vh",
                overflowY: "auto",
                background: "#fff",
                color: "#111",
                borderRadius: "16px",
                padding: "20px",
                boxShadow: "0 20px 60px rgba(0,0,0,0.35)",
              }}
            >
              <button
                type="button"
                onClick={() => setShowLogin(false)}
                aria-label="Close sign in"
                style={{
                  display: "block",
                  marginLeft: "auto",
                  border: 0,
                  background: "transparent",
                  color: "#555",
                  fontSize: "24px",
                  cursor: "pointer",
                  lineHeight: 1,
                }}
              >
                ×
              </button>
              <Login />
            </div>
          </div>
        )}

        <footer className="footer">
          <span>SignalSync Traffic Management</span>
          <span>Frontend dashboard · ESP32 status via Firebase</span>
        </footer>
      </main>
    </div>
  );
}

export default App;

