# Remote microphone

Apollo can receive a microphone from an iPhone or a Mac running Calliope. The voice
arrives on the PC as the Steam Streaming Microphone, which Steam voice chat, games, and
Discord read as an ordinary microphone. Moonlight is not involved and needs no change.

## Requirements

- Apollo on Windows.
- Steam installed on the PC. Steam supplies the Steam Streaming Microphone driver.
- Calliope on an iPhone or a Mac on the same network as the PC.

## Pair a microphone

1. Open Calliope and choose the PC. Calliope shows a 4-digit PIN.
2. On the PC, open the Apollo web UI and go to the PIN page. The tray notice
   "Incoming microphone pairing request" opens it directly.
3. Choose the Microphone tab. Enter the PIN and, optionally, a name for the device.
4. Select Send. The device appears under Microphones at the bottom of the page.

A PIN is valid for 120 seconds. After three wrong PINs, Apollo cancels the request.
Tap Get a new PIN in Calliope to start again.

## Use the microphone

Tap Connect in Calliope. While connected:

- Apollo makes the Steam Streaming Microphone the default microphone on the PC.
- The tray shows "Microphone connected" with the device name.
- The Microphones list shows Connected beside the device.

When Calliope disconnects, Apollo restores the previous default microphone. Apollo also
restores it on exit, and at the next start after a crash.

One microphone is connected at a time. When a second paired device connects, it
replaces the first, and the first shows a message.

## Remove a microphone

On the PIN page, under Microphones, select the delete button beside the device. A
connected device disconnects immediately.

## Network

Calliope sends audio to UDP port 48002 when Apollo uses the default base port. The port
is the base port plus 13. The firewall rule that the Apollo installer adds already covers it.

## Home automation

`GET /api/mic/list` lists paired microphones and marks the connected one. The read-only
API key can read it. See [API](api.md).

## Troubleshooting

| Message | Action |
|---|---|
| Apollo cannot find the Steam Streaming Microphone | Install Steam on the PC, then connect again |
| Another program is blocking the Steam Streaming Microphone | Close Steam Remote Play or the program using the device, then connect again |
| This host cannot receive a microphone | The PC runs Apollo on Linux or macOS. The microphone needs Apollo on Windows |
| Apollo is still opening the microphone | The first connection installs the Steam driver, which takes several seconds. Connect again. The Apollo web UI does not respond during that time |
| The PC lists the device, and Calliope still waits for the PIN | The pairing answer did not reach Calliope. Remove the device under Microphones, then pair again |
