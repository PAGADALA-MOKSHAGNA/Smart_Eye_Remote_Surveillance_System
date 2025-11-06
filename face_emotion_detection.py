import cv2
from deepface import DeepFace

# Load OpenCV's face detector (Haar Cascade)
face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")

# Open webcam
cap = cv2.VideoCapture(0)

if not cap.isOpened():
    print("Error: Could not open webcam.")
    exit()

while True:
    ret, frame = cap.read()
    if not ret:
        print("Failed to capture frame")
        break

    # Convert frame to grayscale for face detection
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    
    # Detect multiple faces
    faces = face_cascade.detectMultiScale(gray, scaleFactor=1.2, minNeighbors=7, minSize=(30, 30))

    for (x, y, w, h) in faces:
        # Extract the face ROI
        face_roi = frame[y:y+h, x:x+w]

        try:
            # Perform emotion analysis directly on face ROI (no saving to disk)
            analysis = DeepFace.analyze(img_path=face_roi, actions=['emotion'], enforce_detection=False)

            if analysis:
                dominant_emotion = analysis[0]['dominant_emotion']

                # Draw bounding box and label emotion
                cv2.rectangle(frame, (x, y), (x+w, y+h), (0, 255, 0), 2)
                cv2.putText(frame, dominant_emotion, (x, y - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)

        except Exception as e:
            print(f"Error analyzing face: {e}")

    # Display the frame
    cv2.imshow("Real-Time Multi-Face Emotion Detection", frame)

    # Press 'q' to quit
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

# Release resources
cap.release()
cv2.destroyAllWindows()