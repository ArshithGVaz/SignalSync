
import { useEffect, useState } from "react";
import { onValue, ref } from "firebase/database";
import { db } from "../firebase/config";

export default function useTrafficLight(enabled = false) {
  const [trafficConfig, setTrafficConfig] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);

  useEffect(() => {
    // Public visitors do not need to read trafficLight.
    if (!enabled) {
      setTrafficConfig(null);
      setLoading(false);
      setError(null);
      return;
    }

    setLoading(true);
    setError(null);

    const trafficRef = ref(db, "trafficLight");

    const unsubscribe = onValue(
      trafficRef,
      (snapshot) => {
        if (snapshot.exists()) {
          setTrafficConfig(snapshot.val());
          setError(null);
        } else {
          setTrafficConfig(null);
          setError("No trafficLight configuration found in Firebase.");
        }

        setLoading(false);
      },
      (firebaseError) => {
        console.error("Firebase read error:", firebaseError);
        setError(firebaseError.message);
        setLoading(false);
      }
    );

    return () => unsubscribe();
  }, [enabled]);

  return { trafficConfig, loading, error };
}
