# Personal Assistant Device
An ESP32-CAM based Personal assistant device based off a local network AI backend. The aim is to have a dynamic personal assistant device that runs generic code, interacting with a far more intelligent backend that manages the logic and needs of the user through AI.
# Current Status and Future work
Current status:
- Successfully wired microphone, LCD and camera and verified function independently (although camera is unused at the moment)
- Solved audio transfer issues.
- calendar correctly updates and deletes events when prompted verbally
- To-do list updates when prompted
- Image lookup pipeline works
- AI backend is demonstrably reliable.
Short term improvement goals:
- Hermes agent has a habit of assuming it is May 2022 despite being accurately fed current date and time. need to investigate why it updates like this.
- Image Lookup API works, but likely avoids copyrighted material. I would like the device to have more general lookup capabilities regardless, so I intend to investigate giving the model browser access rather than simply an image search API.
- Audio recordings and images are saved locally to laptop for every response. While this may be a useful debug tool, it also generates many unnecesarry files that need constant clearing. Should not be difficult to remove this functionality.
- Calendar interface is clean, and I like the message banner showing the AI response, however significant portions of the text get cut off and the actual calendar tool is not as user friendly as I would like (Text too small, unable to display a specific date unless you add an event to it, etc.)
- Deletion of events in calendar require exact event names, and dates sometimes. Even with the addition of a tool intended to allow Hermes to inspect the file and find relevant events, the model does not seem to be able to go find information and needs explicit event titles.
- Boot screens are outdated (mainly "calendar loading screen") should improve these display styles.
- no way to remove to-do list items or mark them as done.
Long term stretch goals:
The ultimate goal of this is to make a fully "generic" device that connects to an AI backend where all the logic and decision making can happen. The vision is that the generic device simply knows what signals it can send, and knows what output capabilities it has. in this case the data it can send is only an audio file, but eventually I hope to expand this to a camera, and other useful sensors. The backend of this then would ask the device for relevant sensor data, and all the interpretation can be done locally on a device designated as the "AI brain" of the project. I imagine being able to say to say something like "This device should now work like a voice memo recorder" or later "this device is now a digital camera", and in response a heavier agentic framework should be able to actively write its own tools to fulfill requests of the user. With this in mind I imagine the long term goals as follows.
- Fully Generalize code on ESP32 to be exclusively an input data forwarding machine, and an output effector, with video and camera capabilities.
- Explore utilizing both cores of the processor with dual threaded code, where one thread can handle output data transmission and the other core can handle input data communication.
- Develop better AI system prompts and use better harnesses and more agents to create a more capable backend that can create/find skills that it needs.
- Deep dive on the memory management to figure out a buffering strategy that will let the device communicate its analog signals effectively. (currently the auido file is limited in duration because it is just allowed to fill short-term memory).
