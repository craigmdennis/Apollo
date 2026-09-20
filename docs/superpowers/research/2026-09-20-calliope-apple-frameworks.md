# Calliope on Apple frameworks: research findings

**Status:** Research. Written to support an implementation plan for the Calliope
remote microphone sidecar described in
`docs/superpowers/specs/2026-09-20-calliope-mic-sidecar-design.md`.

Target platforms are iOS 17 and macOS 14. Every claim below carries a source.
Claims are separated into "Verified from source" and "Inferred". Code is marked
either quoted or written for this report.

Two findings contradict the approved design and are called out where they occur:
voice processing cannot be combined with the requirement that Bluetooth is never
an input (question 4), and Apple's Opus encoder is suitable, which settles the
open question in the design's milestone 3 (question 1).

## Local test environment

Several answers below were settled by running code rather than by reading
documentation. The machine used was macOS with Xcode 27.0 (build 27A266a), SDK
`MacOSX.sdk`, and `ffmpeg` 7.x from Homebrew providing an independent RFC 6716
decoder and a `libopus` decoder. Commands and their output are reproduced in the
relevant sections. This validates the macOS behaviour directly; the iOS
behaviour of the same API is inferred from the shared SDK and listed under
"Still unverified, needs a device test".

## Question 1: Opus encoding with Apple frameworks

### Verified from source

- `kAudioFormatOpus` exists in the shared CoreAudioTypes header and is defined as
  the four-character code `'opus'`. The header comment reads "Opus codec, has no
  flags." Local SDK path:
  `MacOSX.sdk/System/Library/Frameworks/CoreAudioTypes.framework/Headers/CoreAudioBaseTypes.h`,
  lines 386-387 and 432. Documented at
  https://developer.apple.com/documentation/coreaudiotypes/kaudioformatopus
- The same constant is present in the iOS SDK header
  (`iPhoneOS27.0.sdk/System/Library/Frameworks/CoreAudioTypes.framework/Headers/CoreAudioBaseTypes.h`),
  verified locally with `grep`. CoreAudioTypes is a shared framework and the
  constant is not annotated with any platform restriction.
- An Opus **encoder**, not merely a decoder, is present on the system. The stock
  `afconvert` tool lists `'opus'` as a writable data format for the `caff` and
  `mp4f` containers, and encoding succeeds:

  ```
  $ afconvert -f caff -d opus@48000 -c 1 -b 32000 pcm48.caf out.caf
  $ afinfo out.caf
  Data format: 1 ch, 48000 Hz, opus (0x00000000) 0 bits/channel,
               0 bytes/packet, 960 frames/packet, 0 bytes/frame
  bit rate: 30000 bits per second
  audio 71638 valid frames + 312 priming + 50 remainder = 72000
  ```

  (Output quoted from a local run.) Note `960 frames/packet` is the encoder's own
  default at 48 kHz, and the encoder reports a 312-frame priming amount.
- `AVAudioConverter` constructs successfully for a 48 kHz mono float input to a
  `kAudioFormatOpus` output with `mFramesPerPacket = 960`, and produces exactly
  one Opus packet per 960-frame input buffer. Verified by compiling and running a
  probe program locally with `swiftc`. Output quoted from that run:

  ```
  opusFmt: <AVAudioFormat: 1 ch, 48000 Hz, opus (0x00000000) 0 bits/channel,
            0 bytes/packet, 960 frames/packet, 0 bytes/frame>
  enc.bitRate = 32000
  maximumOutputPacketSize = 750
  applicableEncodeBitRates = [6000, 7000, 8000, 12000, 16000, 20000, 24000,
                              28000, 32000, 40000, 50000, 60000, 80000,
                              100000, 120000, 160000, 256000, 512000]
  magicCookie bytes = 28
  packetCount=1 pd=true
  packets=50 (expect 50 for 1 s)  sizes[0..<5]=[119, 73, 84, 88, 88]
  TOC=0x78 config=15 stereo=0 frameCountCode=0
  ```

- The packets are standard, self-delimited-free RFC 6716 packets carrying exactly
  one 20 ms frame. The first byte of each packet is the Opus TOC byte `0x78`,
  which decodes as config 15 (Hybrid mode, fullband, 20 ms), stereo bit 0 (mono),
  frame count code 0 (exactly one frame in the packet). The TOC layout is
  specified in RFC 6716 section 3.1,
  https://datatracker.ietf.org/doc/html/rfc6716#section-3.1
- Those exact packets, taken from the streaming path rather than from a file,
  decode with stock `libopus`. The 50 packets produced by `AVAudioConverter` were
  written out length-prefixed, re-wrapped into an Ogg Opus stream, and decoded:

  ```
  $ ffmpeg -c:a libopus -i reconstructed.opus -f f32le -ar 48000 -ac 1 out.raw
  decoded frames: 47688   peak: 0.5316   rms: 0.3549
  ```

  (Output quoted from a local run.) 47688 is exactly 48000 minus the 312-frame
  preskip, no decode errors were reported, and the decoded signal retains the
  amplitude of the 440 Hz test tone. The same file also decodes with ffmpeg's
  independent native Opus decoder, producing byte-identical output. Two
  independent decoders accepting the bytes is strong evidence of standards
  compliance.
- Bitrate is set with `AVAudioConverter.bitRate`. Apple's documentation for the
  property states "This value only applies when encoding."
  https://developer.apple.com/documentation/avfaudio/avaudioconverter/bitrate
  The set of accepted values is readable from
  `AVAudioConverter.applicableEncodeBitRates`; the design's target of about
  32 kbps is in the list exactly (32000).
- `AVAudioCompressedBuffer` is the output buffer type. Its initialiser is
  `init(format:packetCapacity:maximumPacketSize:)`, and it exposes `packetCount`,
  `byteLength` (available macOS 10.13 / iOS 11.0), `packetDescriptions` and
  `data`. Verified in the local SDK header
  `AVFAudio.framework/Headers/AVAudioBuffer.h` lines 186-254, and documented at
  https://developer.apple.com/documentation/avfaudio/avaudiocompressedbuffer
- Apple publishes an official example of exactly this conversion. An Apple
  Frameworks Engineer posted a PCM-to-Opus `AVAudioConverter` example on the
  developer forums, using `mSampleRate = 48000`, `mFormatID = kAudioFormatOpus`,
  `mChannelsPerFrame = 1`, `mFramesPerPacket = 960`, and
  `AVAudioCompressedBuffer(format:packetCapacity: 1, maximumPacketSize:
  converter.maximumOutputPacketSize)`.
  https://developer.apple.com/forums/thread/763362
  That thread is also the reply an Apple engineer gave in December 2024 to the
  older unanswered Opus question at
  https://developer.apple.com/forums/thread/127317
- The encoder produces a 28-byte magic cookie
  (`AVAudioConverter.magicCookie`; "Encoders will produce a magicCookie",
  `AVAudioConverter.h` line 204). Inspection of the CAF container shows it stored
  in a `kuki` chunk separate from the `data` chunk. It does not begin with
  `OpusHead` and is an AudioToolbox-internal cookie.

### Inferred

- The 312-frame priming figure matches libopus's own encoder lookahead at 48 kHz
  (`OPUS_GET_LOOKAHEAD` returns 312 for a 48 kHz encoder), which together with
  the Hybrid-mode TOC strongly suggests Apple's implementation is libopus or a
  derivative. This is circumstantial, but it aligns with the decode results.
- The priming frames are a constant algorithmic delay, not extra packets. The
  streaming path emits one packet per 960-frame input and no priming packets, as
  the probe run shows (50 packets for 50 input chunks). For Calliope this means a
  fixed additional latency of 312 frames, 6.5 ms, and nothing the wire protocol
  must carry or the host must strip. The host decoder should not skip any samples.
- The magic cookie must **not** be sent on the wire. The Apollo host configures
  its `libopus` decoder explicitly for 48 kHz mono, so it needs no cookie, and
  the design's packet layout has no field for one.
- Apple's forum example writes `outputStream.write(opusBuffer.data, maxLength:
  128)` with a hardcoded 128. That is a defect in the example. The correct length
  is `opusBuffer.byteLength`, or equivalently the `mDataByteSize` of the single
  packet description. The probe run confirms real packets range from 73 to 119
  bytes at 32 kbps, so a hardcoded 128 would both overrun and pad.

### Recommendation

**Use Apple's `AVAudioConverter` Opus encoder. Do not add a libopus dependency
to Calliope.** The design's milestone-3 contingency ("Milestone 3 tests whether
it produces exact 20 ms packets. When it does not, libopus through Swift Package
Manager replaces it.") is resolved in Apple's favour on macOS by direct test: the
packets are exactly 20 ms, exactly one per output buffer, mono, and decodable by
stock libopus.

The contingency should be retained for iOS only, until the same probe is run on a
device. If iOS turns out to lack the encoder, the fallback package is
`sbooth/opus-binary-xcframework`, an Opus XCFramework packaged for SwiftPM,
https://swiftpackageindex.com/sbooth/opus-binary-xcframework — libopus itself is
BSD-3-Clause, https://opus-codec.org/license/ . `ybrid/opus-swift` is an
alternative but its published platform support tops out at iOS 14 and macOS 11,
https://github.com/ybrid/opus-swift . Keeping the encoder behind a small protocol
in `MicCore` makes the swap cheap if the device test fails.

### Encoder configuration for the plan

Written for this report, condensed from the verified probe:

```swift
var desc = AudioStreamBasicDescription()
desc.mSampleRate       = 48000
desc.mFormatID         = kAudioFormatOpus
desc.mChannelsPerFrame = 1
desc.mFramesPerPacket  = 960          // 20 ms; also the encoder's default
let opusFormat = AVAudioFormat(streamDescription: &desc)!

let encoder = AVAudioConverter(from: pcm48kMono, to: opusFormat)!
encoder.bitRate = 32000               // present in applicableEncodeBitRates

// per 960-frame input chunk:
let out = AVAudioCompressedBuffer(format: opusFormat,
                                  packetCapacity: 1,
                                  maximumPacketSize: encoder.maximumOutputPacketSize)
var supplied = false
let status = encoder.convert(to: out, error: &err) { _, inStatus in
    if supplied { inStatus.pointee = .noDataNow; return nil }
    supplied = true; inStatus.pointee = .haveData; return chunk
}
// on .haveData: the packet is out.data, length out.byteLength
```

`maximumOutputPacketSize` measured 750 bytes, so a per-packet send buffer of that
size is sufficient and the design's UDP packet stays well inside any MTU.

## Question 2: capture pipeline

### Verified from source

- **Tap buffer size is advisory.** Apple's parameter text for
  `installTap(onbus:bufferSize:format:block:)`: "**bufferSize** — The size of the
  incoming buffers. The implementation may choose another size." Verified from
  Apple's documentation JSON during this research.
  https://developer.apple.com/documentation/avfaudio/avaudionode/installtap(onbus:buffersize:format:block:)
- The `format:` rule from the same page: "If non-`nil`, the framework applies this
  format to the output bus you specify. An error occurs when attaching to an
  output bus that's already in a connected state. The tap and connection formats
  (if non-`nil`) on the bus need to be identical. Otherwise, the latter operation
  overrides the previous format." The discussion adds: "You can install and remove
  taps while the engine is in a running state. You can install only one tap on any
  bus", and "The framework may invoke the `tapBlock` on a thread other than the
  main thread."
- Passing a tap format whose sample rate differs from the hardware's is a hard
  crash, not a catchable Swift error: `required condition is false:
  format.sampleRate == hwFormat.sampleRate`, raised as an NSException from
  `AVAudioEngineGraph`.
  https://github.com/twilio/voice-quickstart-ios/issues/244
  https://developer.apple.com/forums/thread/711583
- Reading the input format before the I/O unit is instantiated is a separate
  crash: `AVAudioIONodeImpl.mm:365 _GetHWFormat: required condition is false:
  hwFormat`. https://developer.apple.com/forums/thread/73166?page=2
- **One `AVAudioConverter` performs several transformations at once.** Apple's
  overview: "Supported transformations include: PCM float, integer, or bit depth
  conversions; PCM sample rate conversion; PCM interleaving and deinterleaving;
  Encoding PCM to compressed formats; Decoding compressed formats to PCM. A
  single audio converter instance may perform more than one of the above
  transformations."
  https://developer.apple.com/documentation/avfaudio/avaudioconverter
- **Channel reduction is a remap by default and must be switched to a mix.** The
  SDK header for `AVAudioConverter.downmix` reads: "If YES and channel remapping
  is necessary, then channels will be mixed as appropriate instead of remapped.
  **Default value is NO.**" Verified locally in
  `AVFAudio.framework/Headers/AVAudioConverter.h` lines 211-215. Measured
  behaviour, from a local probe run, 48 kHz stereo to 48 kHz mono:

  ```
  downmix=OFF  L=+1.0 R=+1.0 -> +1.000
  downmix=OFF  L=+1.0 R=-1.0 -> +1.000     <- right channel discarded
  downmix=OFF  L=+0.0 R=+1.0 -> +0.000
  downmix=ON   L=+1.0 R=+1.0 -> +1.000
  downmix=ON   L=+1.0 R=-1.0 -> +0.000     <- true L+R mix
  downmix=ON   L=+1.0 R=+0.0 -> +0.500
  downmix=ON   L=+0.0 R=+1.0 -> +0.500
  ```

  Leaving `downmix` at its default on a stereo input silently discards everything
  on the right channel.
- `convert(to:from:)` cannot perform sample-rate conversion; the input-block form
  is required. TN3136 states that the simple method "does not require callbacks
  to convert between PCM audio buffers, but cannot handle sample rate
  conversions."
  https://developer.apple.com/documentation/technotes/tn3136-avaudioconverter-performing-sample-rate-conversions
- TN3136 also gives Apple's recommended shape for variable input sizes: "When
  converting between sample rates, the total number of input frames may be hard
  to predict. One way to cope with variable buffer sizes while providing input to
  `AVAudioConverter` is to fill the input buffers on a sample-by-sample basis",
  reusing one source buffer refilled from a provider inside the block rather than
  allocating per callback. Same URL.
- Input block status cases: `haveData` is "the normal case where you supply data
  to the converter", `noDataNow` is "you're out of data", `endOfStream` is
  "you're at the end of an audio stream".
  https://developer.apple.com/documentation/avfaudio/avaudioconverterinputstatus
  The header adds the operative rule: when out of data for now, set the packet
  count to 0 and return `NoDataNow`, "and the conversion routine will return as
  much output as could be converted with the input already supplied". Verified
  locally in `AVAudioConverter.h` lines 127-154.
- `convert(to:error:withInputFrom:)` "attempts to fill the buffer to its
  capacity. On return, the buffer's length indicates the number of sample frames
  the framework successfully converts."
  https://developer.apple.com/documentation/avfaudio/avaudioconverter/convert(to:error:withinputfrom:)
- **The converter re-chunks to the Opus frame boundary internally.** A local probe
  drove 44.1 kHz stereo input in 512-frame chunks, deliberately matching neither
  960 nor the requested packet count, through a converter chain to 48 kHz mono
  Opus at 960 frames per packet. 60 calls of 512 frames is 30720 frames at
  44.1 kHz, about 33.4 packets at 48 kHz, and the converter produced exactly 34
  packets, each with `packetCount == 1`. Input rates of 8, 16, 24, 44.1 and
  48 kHz all worked, mono and stereo.
- Constant bitrate produces deterministic packet sizes. With
  `bitRateStrategy = AVAudioBitRateStrategy_Constant` set before `bitRate`, local
  probes measured exactly 40, 60, 80 and 160 bytes per 20 ms packet at 16000,
  24000, 32000 and 64000 bps respectively. Left at the default
  (`AVAudioBitRateStrategy_Variable`, `bitRate` reporting -1000 as an unsigned
  value) the size varies and a requested 24000 overshot to about 31200 bps.
  https://developer.apple.com/documentation/avfaudio/avaudioconverter/bitratestrategy
- All six legal Opus frame durations are accepted for `mFramesPerPacket` (120,
  240, 480, 960, 1920, 2880), and values above 960 are emitted as standard
  multi-frame packets with TOC frame-count code 2 or 3. An illegal value such as
  1024 makes `AVAudioConverter(from:to:)` return nil rather than throwing.
  Measured locally.
- **Decode-side packet loss concealment is not exposed.** There is no way to tell
  `AVAudioConverter` that a packet was lost, the way
  `opus_decode_float(dec, NULL, 0, …)` does. The forum thread has been open and
  unanswered since 2022, with a second developer confirming in May 2025.
  https://developer.apple.com/forums/thread/699929
  Opus in-band FEC, DTX, complexity and application mode are likewise not exposed.
- `AVAudioEngineConfigurationChange` behaviour is as quoted in question 5: the
  engine stops and uninitialises itself, nodes keep their previous formats, and
  the app must re-establish connections.
  https://developer.apple.com/documentation/avfaudio/avaudioengineconfigurationchangenotification

### Inferred

- **No hand-written ring buffer or resampler is needed.** The recommended pipeline
  is: install the tap with the node's own hardware format, feed each tap buffer
  into one `AVAudioConverter` whose output format is the Opus format, and emit a
  datagram for each packet it returns. The converter absorbs rate conversion,
  channel reduction and re-chunking to exactly 960 frames. This is the single
  biggest simplification available to the plan and it removes the design's
  implied accumulator.
- Set `converter.downmix = true` whenever the input may be stereo. This is a
  one-line guard against silently losing the right channel, and the design should
  state it explicitly.
- Set `bitRateStrategy` to constant before `bitRate`. Apple's own sample sets it
  first, and constant bitrate gives fixed-size UDP packets, which simplifies the
  host's buffer handling and makes packet loss easier to reason about. The
  reverse ordering was not tested.
- Most `convert` calls in a per-tap-buffer loop return an input-ran-dry status
  with a zero byte length; that is normal. The loop should emit a datagram only
  when the status is `.haveData` and `byteLength > 0`.
- The safe start-up order is: configure and activate the audio session (iOS) or
  set the device on the input node (macOS), touch `engine.inputNode` to force I/O
  instantiation, then read `outputFormat(forBus: 0)`, then install the tap with
  that format. Installing with a mismatched format is an uncatchable crash, so
  the hardware format must never be assumed.
- `maximumOutputPacketSize` measured 750 bytes in every configuration tried,
  including 2880 frames per packet, so it appears to be a per-format constant
  rather than derived from bitrate. It should still be read from the property
  rather than hardcoded.
- The absence of decode-side PLC does not affect Calliope, which only encodes.
  The design already places loss concealment on the Apollo host, which uses
  libopus directly. This only becomes a reason to switch if Calliope ever needs
  to decode.

### If the encoder must be replaced

Should the iOS device test fail, the best-maintained libopus wrapper found is
`alta/swift-opus`: BSD-3-Clause, last code commit June 2024, declaring
`platforms: [.macOS(.v10_12), .iOS(.v12), .tvOS(.v12), .watchOS(.v6)]`, well
under the targets here. It vendors libopus C source as a git submodule and
compiles it with SwiftPM rather than shipping an xcframework, so device,
simulator and Apple Silicon slices all fall out of the normal build; xcframework
based packages are where simulator and arm64 slices tend to go missing. Its API
is `Opus.Encoder(format:application:)` and `encode(_:to:)` over
`AVAudioPCMBuffer`. https://github.com/alta/swift-opus
A worked `AVAudioEngine` example: https://github.com/narner/SwiftOpusAudioDemo

Rejected alternatives: `ybrid/opus-swift` (last push July 2022, no SPDX license,
xcframework based, README caps at iOS 14 / macOS 11.5,
https://github.com/ybrid/opus-swift), `HealsCodes/opus-swift` (one star, no SPDX
license, https://github.com/HealsCodes/opus-swift). LiveKit's Swift SDK and
Signal-iOS both consume a prebuilt WebRTC xcframework with libopus inside rather
than exposing an Opus API, so neither is reusable as a codec dependency
(https://github.com/livekit/client-sdk-swift).

## Question 3: iOS audio session, Bluetooth, and AirPods

### Verified from source

- The `.record` category is correct. Apple: "The category for recording audio
  while also silencing playback audio. This category has the effect of silencing
  virtually all output on the system, for as long as the session is active. … To
  continue recording audio when your app transitions to the background (for
  example, when the screen locks), add the `audio` value to the
  `UIBackgroundModes` key in your information property list file."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/category-swift.struct/record
- **Omitting the Bluetooth option does guarantee that HFP inputs are unavailable.**
  Apple: "You're required to set this option to allow routing audio input and
  output to a paired Bluetooth Hands-Free Profile (HFP) device. If you clear this
  option, paired Bluetooth HFP devices don't show up as available audio input
  routes." and "You can set this option only if the audio session category is
  `playAndRecord` or `record`."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/allowbluetooth
- `availableInputs` depends on the configuration: "The active audio session
  category and mode determine the number of inputs this property returns."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/availableinputs
- Mode `.voiceChat` must not be used: "Setting this mode also causes the system to
  automatically apply the `allowBluetoothHFP` category option."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/mode-swift.struct/voicechat
- `.allowBluetoothA2DP` is irrelevant and automatically cleared: "A2DP is a
  stereo, output-only profile… Audio sessions using the `multiRoute` or `record`
  categories implicitly clear this option."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/allowbluetootha2dp
- `.defaultToSpeaker` is not usable with `.record`: "You can set this option only
  when using the `playAndRecord` category."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/defaulttospeaker
- The iOS 26 SDK renames `allowBluetooth` to `allowBluetoothHFP`. An Apple DTS
  engineer confirms on the forums that "The behavior of allowBluetooth and
  allowBluetoothHFP are exactly the same", and that the accompanying
  "deprecated in iOS 8.0" availability marking is an Apple mistake for which a
  bug report was requested. https://developer.apple.com/forums/thread/797379
  Developer corroboration of the Xcode 26 warning:
  https://forums.swift.org/t/xcode-26-avaudiosession-categoryoptions-allowbluetooth-deprecated/80956
  Option reference:
  https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/allowbluetoothhfp
- A further iOS 26 option to avoid: `bluetoothHighQualityRecording`, which
  "enables full-bandwidth audio when the Bluetooth route supports it, such as on
  certain AirPods models".
  https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/bluetoothhighqualityrecording
- `setPreferredInput(_:)` has a strict ordering rule: "The value of the `inPort`
  parameter must be one of the `AVAudioSessionPortDescription` objects in the
  `availableInputs` array. … You must set a preferred input port only after
  setting the audio session's category and mode and activating the session."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/setpreferredinput(_:)
- Mode `.measurement` "minimize[s] the amount of system-supplied signal
  processing to input and output signals. If recording on devices with more than
  one built-in microphone, the session uses the primary microphone."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/mode-swift.struct/measurement
- Preferred sample rate and buffer duration are requests, not guarantees.
  "This method requests a change to the input and output audio sample rate. To
  see the effect of this change, use the `sampleRate` property."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/setpreferredsamplerate(_:)
  and "To determine whether the change has taken effect, use the
  `ioBufferDuration` property."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/setpreferrediobufferduration(_:)
- `setPreferredDataSource(_:)` is a method on `AVAudioSessionPortDescription`,
  not on the session, and carries the same after-activation ordering rule.
  https://developer.apple.com/documentation/avfaudio/avaudiosessionportdescription/setpreferreddatasource(_:)
  The `dataSources` array "may change if you change the audio session's mode".
  https://developer.apple.com/documentation/avfaudio/avaudiosessionportdescription/datasources
- **There is no app-level API to control AirPods automatic switching.** The only
  control is the user setting Settings > Bluetooth > (i) > "Connect to This
  iPhone" > "Automatically" or "When Last Connected to This iPhone".
  https://support.apple.com/en-us/104988 and
  https://support.apple.com/guide/airpods/switch-airpods-between-apple-devices-dev228ba3df8/web
- Apple describes the automatic-switching trigger in terms of listening and
  playback, and states that "Sound should not switch from one device to another
  automatically if you're in a conversation, like a phone call, a FaceTime call,
  or a video conference." https://support.apple.com/en-us/104988

### Inferred

- A `.record` session with empty options should not pull AirPods from the Apple
  TV, for two reasons: `.record` silences all output so there is no playback for
  the AirPods to become the route for, and with the HFP option cleared the
  AirPods cannot appear in `availableInputs` at all. No Apple documentation
  states this directly, and no DTS answer was found on the question. This is the
  single highest-risk assumption in the design and needs a physical test.
- `setPreferredInput` to the built-in mic is redundant when options are empty,
  because nothing else can be present except a wired or USB input. It is still
  worth keeping: it costs three lines and it is what stops a wired headset
  silently taking over mid-session.
- `.default` versus `.measurement` is the only real mode decision. `.measurement`
  deterministically selects the primary built-in microphone and minimises system
  DSP; `.default` leaves Apple's input conditioning on, which tends to be better
  for voice chat. Since the design disables voice processing (question 4), some
  system conditioning is desirable, so `.default` is the better starting point.

### Configuration for the plan

Written for this report:

```swift
let s = AVAudioSession.sharedInstance()
try s.setCategory(.record, mode: .default, options: [])   // empty options
try s.setPreferredSampleRate(48_000)
try s.setPreferredIOBufferDuration(0.005)
try s.setActive(true)
// only after activation:
if let mic = s.availableInputs?.first(where: { $0.portType == .builtInMic }) {
    try s.setPreferredInput(mic)
}
```

Passing `options: []` also sidesteps the `allowBluetooth` to `allowBluetoothHFP`
rename entirely: neither symbol is referenced, so no deprecation warning appears
and no availability shim is needed.

## Question 4: voice processing on iOS

**This section contradicts the approved design.** The design states "Voice
processing is enabled. It supplies noise suppression and makes Voice Isolation
available in Control Center." That cannot be combined with the requirement that
Bluetooth is never used as an input.

### Verified from source

- Voice processing requires both an input and an output node. Apple's header
  comment for `-[AVAudioIONode setVoiceProcessingEnabled:error:]`: "Voice
  processing requires both input and output nodes to be in the voice processing
  mode. Enabling this mode on either of the IO nodes automatically enables it on
  the other IO node. Voice processing is only supported when the engine is
  rendering to the audio device and not in the manual rendering mode. Voice
  processing can only be enabled or disabled when the engine is in a stopped
  state."
  https://github.com/xybp888/iOS-SDKs/blob/master/iPhoneOS17.0.sdk/System/Library/Frameworks/AVFAudio.framework/Headers/AVAudioIONode.h
- WWDC19 session 510 states the same: "This requires that both input and output
  nodes are in the voice processing mode… Voice processing cannot be enabled
  dynamically, which means the engine needs to be in a stop state when enabling
  the mode… Voice processing is only available when rendering to an audio device,
  not a manual rendering mode."
  https://developer.apple.com/videos/play/wwdc2019/510
- `.record` "has the effect of silencing virtually all output on the system, for
  as long as the session is active", so it provides no output path.
  https://developer.apple.com/documentation/avfaudio/avaudiosession/category-swift.struct/record
- Moving to `.playAndRecord` to obtain an output path triggers the trap: "When an
  app uses this Audio Unit [Voice I/O] without explicitly setting its mode to a
  chat variant (voice, video, or game), the session sets the `voiceChat` mode
  implicitly", and `voiceChat` "causes the system to automatically apply the
  `allowBluetoothHFP` category option."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/mode-swift.struct/voicechat
- Enabling voice processing changes the input node format, and increases rather
  than reduces the channel count. Reported on the forums: 1 channel at 48000 Hz
  becoming 5 channels at 44100 Hz, and 1 channel at 44100 becoming 3 channels.
  The working mitigation in that thread was to pass an explicit desired format to
  `installTap(onBus:bufferSize:format:)`; a manual `AVAudioConverter` over the
  multi-channel buffer produced silence.
  https://developer.apple.com/forums/thread/771530
  Corroboration that the engine stops itself after the format change:
  https://snakamura.github.io/log/2024/11/audio_engine.html
- Voice processing is the API that surfaces the Control Center Mic Mode picker.
  WWDC23 "What's new in voice processing": "Choosing Apple's voice processing
  APIs also grants users full control over the mic mode settings for your app,
  including Standard, Voice Isolation, and Wide Spectrum."
  https://developer.apple.com/videos/play/wwdc2023/10235/
  The user's selection is readable through
  `AVCaptureDevice.preferredMicrophoneMode` and `activeMicrophoneMode`.
  https://developer.apple.com/documentation/avfoundation/avcapturedevice/preferredmicrophonemode
  https://developer.apple.com/documentation/avfoundation/avcapturedevice/activemicrophonemode
- Voice processing ducks other audio, configurably, via
  `AVAudioIONode.voiceProcessingOtherAudioDuckingConfiguration` (iOS 17+).
  https://developer.apple.com/videos/play/wwdc2023/10235/
- Automatic gain control is on by default: the header for
  `voiceProcessingAGCEnabled` reads "Enable automatic gain control on the
  processed microphone uplink signal. Enabled by default."
- Mismatched input and output devices break the voice processing aggregate
  device, for example an AirPods microphone with built-in speakers, failing with
  "client-side input and output formats do not match (err=-10875)".
  https://developer.apple.com/forums/thread/772006
- App-level input muting for a recording app is
  `AVAudioApplication.shared.setInputMuted(_:)`, with
  `setInputMuteStateChangeHandler(_:)` and `inputMuteStateChangeNotification`.
  https://developer.apple.com/documentation/avfaudio/avaudioapplication
- iOS 26 offers `AVInputPickerInteraction` (AVKit), a system input-selection menu
  with live level metering and "a microphone mode selection view, for displaying
  the modes that the input device supports", which does not require voice
  processing. https://developer.apple.com/videos/play/wwdc2025/251/

### Inferred

- `setVoiceProcessingEnabled(true)` under `.record` will either throw or produce
  a non-functional graph, because the VoiceProcessingIO unit instantiates an
  aggregate input-and-output device and `.record` supplies no output. No DTS post
  states the category requirement in as many words, so this is strongly implied
  rather than quoted.
- The chain is therefore: voice processing requires output, output requires
  `.playAndRecord`, `.playAndRecord` with Voice I/O implies `voiceChat` mode, and
  `voiceChat` implies `allowBluetoothHFP`. That is precisely the AirPods capture
  the design exists to avoid.

### Recommendation

**Do not enable voice processing in Calliope on iOS.** The design section "iOS
behaviour" should be amended. What is lost is Apple's noise suppression and the
Control Center Mic Mode picker; what is kept is the guarantee that the AirPods
are never claimed as an input. Since the phone plays no audio during a mic
session, the acoustic echo cancellation that voice processing primarily provides
has nothing to cancel. `AVAudioApplication.setInputMuted(_:)` remains available
for the design's Muted state without voice processing.

## Question 5: background microphone capture on iOS

### Verified from source

- Recording qualifies for the `audio` background mode, and Apple names the lock
  screen explicitly in the `.record` category documentation: "To continue
  recording audio when your app transitions to the background (for example, when
  the screen locks), add the `audio` value to the `UIBackgroundModes` key in your
  information property list file."
  https://developer.apple.com/documentation/avfaudio/avaudiosession/category-swift.struct/record
  Key reference:
  https://developer.apple.com/documentation/bundleresources/information-property-list/uibackgroundmodes
  Note the Xcode capability table describes `audio` as "The app plays audible
  content in the background", which reads as playback-only; the `.record`
  documentation is the authoritative override.
  https://developer.apple.com/documentation/Xcode/configuring-background-execution-modes
- `NSMicrophoneUsageDescription` "is required if your app uses APIs that access
  the device's microphone."
  https://developer.apple.com/documentation/bundleresources/information-property-list/nsmicrophoneusagedescription
- An app cannot *start* recording from the background. The failure is
  `AVAudioSessionErrorCodeCannotStartRecording`, accompanied by the system log
  "CMSUtility_IsAllowedToStartRecording: CMSession: Client <private> with PID 909
  is in the background and doesn't have the entitlement to start recording in the
  background". https://developer.apple.com/forums/thread/120038
- Interruption handling is documented, including the `.shouldResume` option, at
  https://developer.apple.com/documentation/avfaudio/handling-audio-interruptions
  Apple's sample shape, quoted from that page:

  ```swift
  guard let userInfo = notification.userInfo,
        let typeValue = userInfo[AVAudioSessionInterruptionTypeKey] as? UInt,
        let type = AVAudioSession.InterruptionType(rawValue: typeValue) else { return }
  switch type {
  case .began:  // stop / flush; the session is already inactive
  case .ended:
      guard let optionsValue = userInfo[AVAudioSessionInterruptionOptionKey] as? UInt else { return }
      let options = AVAudioSession.InterruptionOptions(rawValue: optionsValue)
      if options.contains(.shouldResume) { /* resume */ }
  default: ()
  }
  ```

  The same page documents `overrideMutedMicrophoneInterruption` and
  `setPrefersNoInterruptionsFromSystemAlerts(_:)`.
- `AVAudioSession.InterruptionReason` cases:
  `.default`, `.builtInMicMuted`, `.routeDisconnected`, `.sceneWasBackgrounded`,
  and `.appWasSuspended`, which is **deprecated**.
  https://developer.apple.com/documentation/avfaudio/avaudiosession/interruptionreason
- `AVAudioSession.RouteChangeReason` cases include `.newDeviceAvailable`
  ("a user action, such as plugging in a headset, has made a preferred audio
  route available"), `.oldDeviceUnavailable`, `.override`, `.categoryChange`,
  `.wakeFromSleep`, `.noSuitableRouteForCategory` and
  `.routeConfigurationChange`.
  https://developer.apple.com/documentation/avfaudio/avaudiosession/routechangereason
- The notification that forces an engine rebuild is
  `AVAudioEngineConfigurationChange`: "When the audio engine's I/O unit observes
  a change to the audio input or output hardware's channel count or sample rate,
  the audio engine stops, uninitializes itself, and issues this notification. The
  nodes remain in an attached and connected state with the previously set
  formats. The app must reestablish connections if the connection formats need to
  change. Note: Don't deallocate the engine from within the client's notification
  handler. The callback happens on an internal dispatch queue and can deadlock
  while trying to tear down the engine synchronously."
  https://developer.apple.com/documentation/foundation/nsnotification/name-swift.struct/avaudioengineconfigurationchange
- The orange microphone indicator is Apple-documented as showing when the
  microphone is in use. https://support.apple.com/en-us/108331

### Inferred

- The app must start the engine and activate the session while in the foreground,
  then allow the screen to lock. If an interruption stops capture while
  backgrounded, the app may be unable to restart until the user foregrounds it.
  The Apollo receiver and the Calliope UI must tolerate a mic stream that stops
  and needs a user tap to resume; this fits the design's existing "Reconnecting"
  and error states.
- On `.ended` with `.shouldResume` the app itself must both call `setActive(true)`
  again and restart `AVAudioEngine`. The system does neither. Apple only
  auto-resumes `AVPlayer`, which it calls out separately.
- On route change, re-apply `setPreferredInput(builtInMic)` and re-read
  `currentRoute.inputs`. Reinstalling the tap is generally not required from the
  route-change handler; the `AVAudioEngineConfigurationChange` notification is
  what signals a genuine rebuild, and it must be handled off the delivering queue.
- No documentation was found stating that iOS terminates a backgrounded app
  holding an active recording session that produces no audio output. A `.record`
  session producing no output is the normal case for that category. The realistic
  termination causes are memory pressure and the session becoming inactive.
- The `voip` background mode should not be used. It implies CallKit and would
  push the app toward `.playAndRecord` and `.voiceChat`, reintroducing the
  Bluetooth problem. The `.record` documentation shows `audio` is sufficient.

## Question 6: macOS input device selection without changing the system default

### Verified from source

- Setting `kAudioOutputUnitProperty_CurrentDevice` on `inputNode.audioUnit` is
  the established technique, and an Apple staff member in the same thread
  confirms AVFoundation offers no higher-level API for choosing a device and that
  wrapping a custom I/O unit into the engine is not possible.
  https://developer.apple.com/forums/thread/71008
  The property is documented in the header as "Scope: Global, Value Type:
  AudioObjectID, Access: read/write", and equals 2000. Verified locally in
  `MacOSX.sdk/System/Library/Frameworks/AudioToolbox.framework/Headers/AudioUnitProperties.h`
  lines 2426 and 2522.
- A Swift-level equivalent exists and predates macOS 14:
  `engine.inputNode.auAudioUnit.setDeviceID(_:)`, declared in the SDK header as
  `- (BOOL)setDeviceID:(AUAudioObjectID)deviceID error:(NSError **)outError;`
  with a companion read-only `deviceID` property. Verified locally in
  `AudioToolbox.framework/Headers/AUAudioUnit.h` lines 1274-1283. The macOS-only
  group (`deviceID`, `isInputEnabled`, `isOutputEnabled`, `canPerformInput`,
  `canPerformOutput`) is macOS 10.13+.
  https://developer.apple.com/documentation/audiotoolbox/auaudiounit
- Apple's ordering rules for an AUHAL come from TN2091: enable IO on the input
  element before setting the device, because "devices can only be set to the
  AUHAL after enabling IO", then set the stream format, then the callback, then
  initialise and start.
  https://developer.apple.com/library/archive/technotes/tn2091/_index.html
  The header confirms output element 0 and input element 1, and that "Output
  units default to output-only operation"
  (`AudioUnitProperties.h` line 2448).
- **On macOS `inputNode` and `outputNode` share one underlying `AUAudioUnit`.**
  Setting the device on one changes the other; the engine throws on start when
  the chosen device lacks both inputs and outputs while both nodes are in use;
  and `outputBusses[0].format` can read zero channels after a device change.
  https://developer.apple.com/forums/thread/133080
  https://developer.apple.com/forums/thread/683348
  https://github.com/AudioKit/AudioKit/issues/2130
- **Touching `outputNode` or `mainMixerNode` is what pulls AirPods into the
  hands-free profile.** macOS builds an implicit aggregate of the AirPods' 2-channel
  48 kHz output device and 1-channel 16 kHz input device: "the AirPods must drop
  their playback quality down to 16kHz whenever the microphone is activated", and
  this "appears to happen automatically when I ask for the AVAudioEngine's
  outputNode, or its mainMixerNode".
  https://supermegaultragroovy.com/2021/01/28/more-on-avaudioengine-airpods/
- Apple Support confirms the general mechanism: Bluetooth has two modes and "When
  Bluetooth switches to the second mode, audio quality is reduced", with the
  remedy being to quit the app using the headset microphone.
  https://support.apple.com/en-hk/102217
  An Apple forum post states "You cannot currently play out via A2DP while
  accepting input via HFP". https://developer.apple.com/forums/thread/5787
- An Apple Media Engineer recommends `AVCaptureDevice` plus `AVCaptureSession`
  with `AVCaptureAudioDataOutput` "as a provider of audio samples" for picking a
  specific input device on macOS. That path binds a device directly and has no
  shared-I/O-unit problem. https://developer.apple.com/forums/thread/775015
- macOS 14 added no new device-selection API; WWDC23's audio session covered
  voice processing and muted-talker detection only.
  https://developer.apple.com/videos/play/wwdc2023/10235/
- Device enumeration constants, all verified locally in
  `CoreAudio.framework/Headers/AudioHardwareBase.h` and `AudioHardware.h`:
  `kAudioHardwarePropertyDevices` = `'dev#'`,
  `kAudioDevicePropertyStreamConfiguration` = `'slay'` (query on
  `kAudioObjectPropertyScopeInput` = `'inpt'` and count channels to identify
  inputs), `kAudioObjectPropertyName` = `'lnam'`,
  `kAudioDevicePropertyDeviceUID` = `'uid '`,
  `kAudioDevicePropertyTransportType` = `'tran'`.
- Transport type values, verified locally in `AudioHardwareBase.h` lines 608-615:
  `kAudioDeviceTransportTypeBuiltIn` = `'bltn'`,
  `kAudioDeviceTransportTypeAggregate` = `'grup'`,
  `kAudioDeviceTransportTypeVirtual` = `'virt'`,
  `kAudioDeviceTransportTypeUSB` = `'usb '`,
  `kAudioDeviceTransportTypeBluetooth` = `'blue'`,
  `kAudioDeviceTransportTypeBluetoothLE` = `'blea'`.
  https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertytransporttype
- Two deprecations to avoid: `kAudioDevicePropertyDeviceNameCFString` is
  deprecated and literally defined as `= kAudioObjectPropertyName`
  (`AudioHardwareDeprecated.h` line 681), and
  `kAudioObjectPropertyElementMaster` is deprecated in favour of
  `kAudioObjectPropertyElementMain` (`AudioHardwareBase.h` line 208).
- `AVCaptureDevice.DeviceType.microphone` is macOS 14.0+, and
  `.builtInMicrophone` is deprecated as of macOS 14.0. Verified locally in
  `AVFoundation.framework/Headers/AVCaptureDevice.h` lines 550, 556, 666, 678.
  `AVCaptureDevice.DiscoverySession` does not necessarily list every input
  device; a virtual driver returned an empty list in one unresolved report.
  https://developer.apple.com/forums/thread/720919
- Entitlements: `com.apple.security.device.audio-input` is the current App
  Sandbox key, and `com.apple.security.device.microphone` is the old name.
  Apple's revision history says "this key's name has since changed to
  com.apple.security.device.audio-input".
  https://developer.apple.com/library/archive/documentation/Miscellaneous/Reference/EntitlementKeyReference/Chapters/RevisionHistory.html
  https://developer.apple.com/library/archive/documentation/Miscellaneous/Reference/EntitlementKeyReference/Chapters/EnablingAppSandbox.html
  `com.apple.security.network.client` is "Network socket for connecting to other
  machines" on the same page. The Hardened Runtime exposes the same audio-input
  item and needs no `com.apple.security.cs.*` exception for plain capture.
  https://developer.apple.com/documentation/xcode/configuring-the-hardened-runtime
- A missing `NSMicrophoneUsageDescription` terminates the process under
  "Namespace TCC". It is a crash, not a denial.
  https://developer.apple.com/forums/thread/691896
- A command-line tool run from Terminal inherits its TCC permission from the
  responsible parent process, so Terminal owns the microphone grant.
  https://developer.apple.com/forums/thread/675773

### Inferred

- Enumerating CoreAudio properties does not trigger the HFP switch. Only starting
  IO on a Bluetooth input device does. This matters because Calliope's macOS
  Input card must list devices, including the AirPods, without degrading them.
- Pointing the AUHAL at the built-in microphone leaves the AirPods in A2DP,
  provided the app never touches `outputNode` or `mainMixerNode`. This combines
  the TN2091 ordering with the AirPods aggregate finding; no single Apple
  statement says it.
- The safest recipe for this capture-only app is therefore: create the engine,
  set the device on `inputNode` before reading `inputFormat(forBus:)` or
  installing a tap, and never reference `outputNode` or `mainMixerNode`. If that
  proves fragile on an input-only device, fall back to `AVCaptureSession`, which
  is Apple's own recommendation and avoids the shared I/O unit entirely.
- The design's macOS Bluetooth warning should be driven by
  `kAudioDevicePropertyTransportType` matching `'blue'` or `'blea'`.
- For the `micsend` CLI, `tccutil reset Microphone com.apple.Terminal` is the
  reset that matters during development.

## Question 7: networking

### Verified from source

- `NWConnection` construction and lifecycle:
  `init(to: NWEndpoint, using: NWParameters)`, the convenience
  `init(host:port:using:)`, `start(queue:)`, `stateUpdateHandler`,
  `currentPath`, `betterPathUpdateHandler`.
  https://developer.apple.com/documentation/network/nwconnection
- `send(content:contentContext:isComplete:completion:)` takes an
  `NWConnection.SendCompletion` of either `.contentProcessed((NWError?) -> Void)`
  ("Provide a completion handler that's invoked when the sent data is processed
  by the stack") or `.idempotent` ("Mark the sent data as idempotent—data that
  can be sent multiple times").
  https://developer.apple.com/documentation/network/nwconnection/sendcompletion
- A UDP `NWConnection` is bidirectional and pongs arrive on `receiveMessage`:
  "Receiving messages allows you to deal with complete datagrams or
  application-layer messages without needing to reconstruct a stream. If you are
  using UDP, receiving a message will deliver a single datagram."
  https://developer.apple.com/documentation/network/nwconnection/receivemessage(completion:)
  One call delivers one message; the handler must call `receiveMessage` again to
  re-arm. https://developer.apple.com/forums/thread/129465
- Apple's DTS networking engineer on flow control at high send rates: "On the
  send side, implement flow control by waiting for the connection to call your
  completion handler before generating and sending more data… Failing to
  implement flow control can result in unbounded memory growth in your app, which
  is particularly problematic on iOS where jetsam will terminate your app if it
  uses too much memory." https://developer.apple.com/forums/thread/747815
- **Two outgoing UDP `NWConnection`s cannot share a local port** in one process;
  the second fails with `EADDRINUSE`. DTS recommends BSD sockets for that case
  (FB13678278). https://developer.apple.com/forums/thread/747815
- `NWParameters.ServiceClass` includes `.interactiveVoice`, "A service type for
  low-delay tolerant, very low-loss tolerant, inelastic flow, and constant packet
  rate connections."
  https://developer.apple.com/documentation/network/nwparameters/serviceclass-swift.enum
  Other relevant parameters: `allowLocalEndpointReuse`, `requiredInterfaceType`,
  `prohibitedInterfaceTypes`, `includePeerToPeer`.
  https://developer.apple.com/documentation/network/nwparameters
- `NWBrowser.Descriptor.bonjour(type:domain:)` (iOS 13+/macOS 10.15+), with
  Apple's example using a trailing dot on the domain: `.bonjour(type: "_ssh._tcp",
  domain: "local.")`.
  https://developer.apple.com/documentation/network/nwbrowser/descriptor-swift.enum/bonjour(type:domain:)
  `NWBrowser.Result` carries `endpoint`, `interfaces` and `metadata`, where
  metadata is `.bonjour(NWTXTRecord)` or `.none`. It carries no addresses and no
  port. https://developer.apple.com/documentation/network/nwbrowser/result
- **Network.framework has no separate Bonjour resolve operation.** Apple's DTS
  engineer: "What you need is the Bonjour 'resolve' operation… Network framework
  has no API for this resolve operation", and "Resolving every service you
  encounter when browsing is a major Bonjour faux pas. The goal is to only
  resolve the service you're connecting to, and then only at the point when you
  do the connection." The documented route is to connect to the `.service`
  endpoint and read `connection.currentPath?.remoteEndpoint`.
  https://developer.apple.com/forums/thread/122638
  A `.service` endpoint can be passed straight to `NWConnection`, and a `.local.`
  hostname also works: https://developer.apple.com/forums/thread/114809
- `NSBonjourServices` takes entries of the form
  `_applicationprotocol._transportprotocol`, and "Include all service types that
  your app expects to use." iOS 14+/macOS 11+.
  https://developer.apple.com/documentation/bundleresources/information-property-list/nsbonjourservices
- `NSLocalNetworkUsageDescription`: "Any app that uses the local network,
  directly or indirectly, should include this description. This includes apps
  that use Bonjour and services implemented with Bonjour, as well as direct
  unicast or multicast connections to local hosts."
  https://developer.apple.com/documentation/bundleresources/information-property-list/nslocalnetworkusagedescription
- TN3179 is the authoritative reference for local network privacy. It states the
  policy applies to iOS/iPadOS 14+, visionOS 1+ and **macOS 15+**; that the
  prompt is triggered automatically by the first local network operation while
  the privilege is undetermined; that **there is no authorization-status API**
  (FB8711182); and that the Simulator does not support local network privacy, so
  it must be tested on a device. It also notes a background app in the
  undetermined state is denied without the alert being shown.
  https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy
- TN3179's two accepted denial heuristics, quoted from the technote: for a
  browser, `NWBrowser` enters `.waiting(.dns(code))` where
  `Int(code) == kDNSServiceErr_PolicyDenied`; for a connection,
  `connection.currentPath?.unsatisfiedReason == .localNetworkDenied` while in
  `.waiting`. Confirmed locally that `kDNSServiceErr_PolicyDenied = -65570` in
  `MacOSX.sdk/usr/include/dns_sd.h` line 801.
- App Sandbox entitlements: `com.apple.security.network.client` for outgoing
  connections
  (https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.network.client)
  and `com.apple.security.network.server`, which gates "whether your app may
  listen for incoming network connections"
  (https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.network.server).
  Discussion of the split: https://developer.apple.com/forums/thread/744961

### Inferred

- One long-lived `NWConnection` sending 50 times per second is the correct
  pattern. A connection per packet would re-run path evaluation and local port
  binding each time. At roughly 80-byte payloads there is no backpressure risk,
  so `.idempotent` is the appropriate completion mode; `.contentProcessed` is
  worth using only if per-packet send errors are wanted as a faster failure
  signal than the design's 3-second pong timeout.
- `batch(_:)` offers nothing here, since one frame is issued per wakeup.
- The connection queue should be a dedicated serial queue at
  `qos: .userInteractive`, not the main queue, because all send completions and
  receives are delivered on it.
- Recommended parameters, written for this report:

  ```swift
  let params = NWParameters.udp
  params.serviceClass = .interactiveVoice
  params.prohibitedInterfaceTypes = [.cellular]
  params.includePeerToPeer = false
  ```

  `prohibitedInterfaceTypes = [.cellular]` is preferable to
  `requiredInterfaceType = .wifi`, which would hard-fail on an
  Ethernet-connected Mac.
- Receiving pongs on a connection Calliope itself initiated does not require
  `com.apple.security.network.server`. That entitlement gates a listening socket,
  and replies on an outgoing flow are part of that flow. `network.client` alone
  is sufficient unless an `NWListener` is ever added.
- Because the design's mic session uses a single UDP flow for audio, pings and
  pongs, the one-local-port limitation above does not bite. It would bite if the
  plan later split control onto a second socket.
- The `.service` endpoint can be handed straight to `NWConnection` for the UDP
  channel, so no resolve is needed there. A concrete host and port is only needed
  for the `URLSession` pairing step, which wants a URL.

## Question 8: TLS pinning of a self-signed certificate

### Verified from source

- The delegate method is
  `urlSession(_:didReceive:completionHandler:)` on `URLSessionDelegate`, which
  handles `NSURLAuthenticationMethodServerTrust` among four schemes.
  https://developer.apple.com/documentation/foundation/urlsessiondelegate/urlsession(_:didreceive:completionhandler:)
  `URLSession.shared` does not support a delegate, so the app must construct its
  own session. https://developer.apple.com/forums/thread/666577
- Apple's DTS engineer publishes the canonical pattern for an app talking to a
  local device with a self-signed certificate, described as the "SSH approach":
  trust on first use, then pin. Quoted from
  https://developer.apple.com/forums/thread/703234 :

  ```swift
  func shouldAllowHTTPSConnection(trust: SecTrust) async -> Bool {
      guard
          let chain = SecTrustCopyCertificateChain(trust) as? [SecCertificate],
          let actual = chain.first
      else { return false }
      guard let expected = self.expectedCertificate else {
          self.expectedCertificate = actual   // A. First connection
          return true
      }
      return CFEqual(expected, actual)        // B. Subsequent connections
  }

  func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge)
      async -> (URLSession.AuthChallengeDisposition, URLCredential?)
  {
      switch challenge.protectionSpace.authenticationMethod {
      case NSURLAuthenticationMethodServerTrust:
          let trust = challenge.protectionSpace.serverTrust!
          guard await self.shouldAllowHTTPSConnection(trust: trust) else {
              return (.cancelAuthenticationChallenge, nil)
          }
          return (.useCredential, URLCredential(trust: trust))
      default:
          return (.performDefaultHandling, nil)
      }
  }
  ```

  Note that `SecTrustEvaluateWithError` is **not** called, the failure
  disposition is `.cancelAuthenticationChallenge`, and the same post warns to key
  the stored certificate on something identifying the device rather than on its
  DNS name or IP address.
- **Trust evaluation should not be attempted on a self-signed leaf.** Apple's DTS
  engineer: "In general, trust evaluation only makes sense when you have
  certificates issued by CAs, where the trust evaluator builds a potentially
  complex chain of trust from the certificate to a set of trusted anchors. If all
  your certificates are self signed, there's very little point doing trust
  evaluation on them." Reported failure modes for self-signed leaves include
  "certificate is not standards compliant" and "certificate is not permitted for
  this usage". https://developer.apple.com/forums/thread/732989
- `SecTrustCopyCertificateChain(_:) -> CFArray?` is available iOS 15.0+ /
  macOS 12.0+; index 0 is the leaf.
  https://developer.apple.com/documentation/security/sectrustcopycertificatechain(_:)
  `SecTrustGetCertificateAtIndex(_:_:)` is deprecated as of iOS 15.0 / macOS 12.0
  with that function named as its replacement.
  https://developer.apple.com/documentation/security/sectrustgetcertificateatindex(_:_:)
- `SecCertificateCopyData(_:) -> CFData` returns "the DER representation of an
  X.509 certificate".
  https://developer.apple.com/documentation/security/seccertificatecopydata(_:)
- **ATS is evaluated separately from, and before, the server-trust delegate.**
  Apple's DTS engineer: "Basic X.509 and TLS trust evaluation are done for all
  TLS connections. ATS is only done on TLS connections made by `URLSession` and
  things layered on top `URLSession`", and "If you rely on loosened security you
  have to disable ATS." https://developer.apple.com/forums/thread/67493
  A delegate therefore cannot rescue a connection ATS itself rejected.
  The same engineer's accessory guide pairs the pinning delegate with
  `NSAllowsLocalNetworking`. https://developer.apple.com/forums/thread/703234
- Loosening ATS does not remove standard trust evaluation:
  `NSAllowsLocalNetworking` "only disables the additional security checking done
  by ATS. NSURLSession still does standard (RFC 2818) server trust evaluation on
  your HTTPS connections." https://developer.apple.com/forums/thread/6205
- **The critical version change for this project.** Quoted from Apple's
  `NSAllowsLocalNetworking` documentation: "The `NSAllowsLocalNetworking` key
  controls whether App Transport Security (ATS) allows your app to connect to
  unqualified domains, `.local` domains, and IP addresses using IPv4 or IPv6…
  In iOS 10 through iOS 16, iPadOS 13.1 through iPadOS 16, and macOS 10.12
  through macOS 13, ATS allows all three of these connections by default… **In
  iOS 17, iPadOS 17, and macOS 14, ATS no longer allows connections to IP
  addresses by default. Add individual IP addresses and classless inter-domain
  routing (CIDR) ranges in the `NSExceptionDomains` dictionary.**" The same page
  states the local networking exception "enable[s] access to unqualified domains,
  `.local` domains, and IP addresses that they would otherwise restrict", and
  recommends setting it to `YES` "as a declaration of intent".
  https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking
  Verified verbatim from Apple's documentation JSON during this research.
- Distinguishing the two failure modes, from Apple's DTS engineer: ATS failures
  surface as TLS errors, typically `NSURLErrorSecureConnectionFailed` (-1200);
  local network privacy failures surface as transport errors. "Playing with ATS
  settings won't help with an LNP error, and vice versa."
  https://developer.apple.com/forums/thread/788044

### Inferred

- Calliope's pinning differs from the DTS sample in one way that simplifies it:
  the fingerprint is not learned on first use but comes from the design's pairing
  exchange, where it is bound into the PIN proof. So the "first connection"
  branch applies only during pairing, and every later request compares a stored
  SHA-256 against the leaf. Comparing hashes rather than `CFEqual` on certificate
  objects is the right choice because the stored value is a hash by design.
- Constant-time comparison of the fingerprint is cosmetic. A certificate hash is
  public data, so a timing oracle leaks nothing an attacker cannot obtain by
  connecting to the host. It costs nothing to write, so it may as well be
  written, but it should not shape the code.
- **Connecting by the host's `.local` name rather than a resolved IP address is
  the better plan.** It avoids the iOS 17 / macOS 14 raw-IP ATS restriction
  entirely, avoids the Bonjour resolve dance, and is what Apple's own examples
  use. `NSAllowsLocalNetworking` should still be set. If a raw IP address must be
  supported, the private CIDR ranges have to be added under `NSExceptionDomains`
  as Apple's documentation instructs.
- The design's stored "host address" should therefore be recorded as the Bonjour
  service name or `.local` hostname, with the IP address treated as a cache. The
  pin itself must be keyed on the host's mic-device UUID from the pairing
  response, not on an address, following the DTS warning above.
- A macOS command-line target built without an `Info.plist` is not subject to ATS
  at all, which makes `micsend` an easier first end-to-end test than the app.
  https://developer.apple.com/forums/thread/701588

### Info.plist for the plan

Written for this report:

```xml
<key>NSLocalNetworkUsageDescription</key>
<string>Calliope finds Apollo on your network so it can send your microphone to your PC.</string>
<key>NSBonjourServices</key>
<array><string>_nvstream._tcp</string></array>
<key>NSMicrophoneUsageDescription</key>
<string>Calliope sends this microphone to Apollo on your PC for voice chat.</string>
<key>NSAppTransportSecurity</key>
<dict><key>NSAllowsLocalNetworking</key><true/></dict>
<key>UIBackgroundModes</key>
<array><string>audio</string></array>
```

## Question 9: CryptoKit

### Verified from source

- `AES.GCM.seal(_:using:nonce:)` accepts a caller-supplied nonce; the parameter
  is optional and "If you don't provide a nonce, the method generates a random
  one". Available iOS 13.0+ / macOS 10.15+, verified from Apple's documentation
  JSON during this research.
  https://developer.apple.com/documentation/cryptokit/aes/gcm/seal(_:using:nonce:)
- The AAD variant exists: `seal(_:using:nonce:authenticating:)`, "Secures
  plaintext with encryption and authentication tag covering both encrypted data
  and additional data." Matching `open(_:using:authenticating:)` also exists.
  https://developer.apple.com/documentation/cryptokit/aes/gcm
- `AES.GCM.Nonce` offers `init()` for a random nonce and
  `init<D: ContiguousBytes>(data: D) throws` for a supplied one.
  https://developer.apple.com/documentation/cryptokit/aes/gcm/nonce
  swift-crypto, which Apple states re-exports CryptoKit's API on Apple platforms,
  defines `static let defaultNonceByteCount = 12`.
  https://github.com/apple/swift-crypto/blob/main/Sources/Crypto/AEADs/AES/GCM/AES-GCM.swift
- `AES.GCM.SealedBox` exposes `nonce`, `ciphertext`, `tag` and `combined`
  separately, and can be built from parts with
  `init<C, T>(nonce:ciphertext:tag:) throws`.
  https://developer.apple.com/documentation/cryptokit/aes/gcm/sealedbox
- The tag is 16 bytes and the size is enforced. swift-crypto defines
  `static let tagByteCount = 16` and the parts initialiser contains
  `guard tag.count == AES.GCM.tagByteCount else { throw
  error(CryptoKitError.incorrectParameterSize) }`. Same swift-crypto URL as above.
- A tag mismatch on `open` throws `CryptoKitError.authenticationFailure`, "The
  authentication tag or signature is incorrect."
  https://developer.apple.com/documentation/cryptokit/cryptokiterror
- `SymmetricKey(data:)` infers the algorithm variant from the data length;
  swift-crypto's `bitCount` is `byteCount * 8`, so 16 bytes gives AES-128. There
  is no separate AES-128 type.
  https://developer.apple.com/documentation/cryptokit/symmetrickey
  https://github.com/apple/swift-crypto/blob/main/Sources/Crypto/Keys/Symmetric/SymmetricKeys.swift
- `HMAC<H>.authenticationCode(for:using:)` has no documented key-length
  constraint, and swift-crypto's implementation is textbook RFC 2104: a key at or
  below the block size is used directly, a longer key is hashed first, the
  remainder is zero-filled, and the inner and outer pads are XORed with `0x36`
  and `0x5c`. No minimum length is enforced, so a 4-byte PIN as UTF-8 produces a
  value identical to any standard HMAC-SHA256 implementation, including the
  host's C++ code.
  https://developer.apple.com/documentation/cryptokit/hmac/authenticationcode(for:using:)
  https://github.com/apple/swift-crypto/blob/main/Sources/Crypto/Message%20Authentication%20Codes/HMAC/HMAC.swift
  https://datatracker.ietf.org/doc/html/rfc2104
- `HashedAuthenticationCode` conforms to `ContiguousBytes` and `Sequence`, so
  `Data(mac)` yields the raw bytes.
  https://developer.apple.com/documentation/cryptokit/hashedauthenticationcode
- `HMAC.isValidAuthenticationCode(_:authenticating:using:)` exists and performs
  the comparison itself. Same authenticationCode URL.
- CryptoKit is iOS 13.0+ / macOS 10.15+ throughout, comfortably within the
  targets. https://developer.apple.com/documentation/cryptokit/

### Inferred

- CryptoKit is usable from a plain macOS command-line tool such as `micsend`.
  Nothing in the AES-GCM, HMAC or hashing surface reads an `Info.plist`, requires
  an entitlement, or touches the keychain, unlike the `SecKey` and Secure Enclave
  paths. Apple's swift-crypto README states that on Apple platforms Swift Crypto
  "compiles down to nothing and simply re-exports the API of CryptoKit".
  https://github.com/apple/swift-crypto
- The design's 12-byte nonce is constructed by Calliope rather than by CryptoKit,
  so `SealedBox.combined` is not useful. The wire format is
  `[tag][ciphertext]`, so the code should take `box.tag` and `box.ciphertext`
  separately and never serialise `combined`.
- On the receive path for pongs, `AES.GCM.open` should be wrapped so that an
  `authenticationFailure` drops the single datagram rather than ending the mic
  session. A corrupted or replayed datagram is expected on UDP, and treating it
  as fatal would produce unexplained mid-session drops.

## Question 10: macOS app shell

### Verified from source

- `MenuBarExtra` is macOS 13.0+, with initialisers taking a title, `systemImage:`,
  `image:` or a `label:` closure, each also available with an
  `isInserted: Binding<Bool>`.
  https://developer.apple.com/documentation/swiftui/menubarextra
- `.menuBarExtraStyle(.window)` is `WindowMenuBarExtraStyle`, described as a
  "popover-like window"; `.menu` is `PullDownMenuBarExtraStyle`.
  https://developer.apple.com/documentation/swiftui/menubarextrastyle
  https://developer.apple.com/documentation/swiftui/windowmenubarextrastyle
- There is **no API to close the `.window` panel programmatically** (FB11984872);
  only `isInserted` is official.
  https://github.com/feedback-assistant/reports/issues/383
  Third-party workaround: https://github.com/orchetect/MenuBarExtraAccess
- `SettingsLink` and `openSettings` are macOS 14+, and both are unreliable from a
  MenuBarExtra-only app under the accessory activation policy. The working fix is
  a hidden `Window` scene declared before `Settings`, combined with temporarily
  switching the activation policy to `.regular`.
  https://developer.apple.com/documentation/swiftui/settingslink
  https://steipete.me/posts/2025/showing-settings-from-macos-menu-bar-items
  Original feedback report FB10184971:
  https://github.com/feedback-assistant/reports/issues/327
- `LSUIElement` is a Boolean Info.plist key.
  https://developer.apple.com/documentation/bundleresources/information-property-list/lsuielement
  `NSApplication.ActivationPolicy.accessory` "corresponds to a value of the
  LSUIElement key … being 1".
  https://developer.apple.com/documentation/appkit/nsapplication/activationpolicy-swift.enum/accessory
- `SMAppService` is macOS 13.0+. `SMAppService.mainApp` needs no helper bundle or
  plist, unlike `agent(plistName:)` and `daemon(plistName:)`. `Status` cases are
  `.notRegistered`, `.enabled`, `.requiresApproval` and `.notFound`; `register()`
  can throw `kSMErrorAlreadyRegistered` and `kSMErrorLaunchDeniedByUser`; and
  `openSystemSettingsLoginItems()` opens the pane where the user can disable it.
  https://developer.apple.com/documentation/servicemanagement/smappservice
  https://developer.apple.com/documentation/servicemanagement/smappservice/status-swift.enum
  https://developer.apple.com/documentation/servicemanagement/smappservice/register()
  Introduced in WWDC22 session 10096:
  https://developer.apple.com/videos/play/wwdc2022/10096/
  "Operation not permitted" threads:
  https://developer.apple.com/forums/thread/762551 and
  https://developer.apple.com/forums/thread/743395
- **Ad-hoc signing breaks background execution.** Under "Sign to Run Locally" an
  app will "lose your right to execute in the background on every restart", and
  Apple's DTS engineer advises using an Apple-issued identity.
  https://developer.apple.com/forums/thread/799910

### Inferred

- Set `LSUIElement` to true as the static configuration for the menu bar app, and
  switch the activation policy at runtime only when showing Settings. A
  MenuBarExtra-only `App` with no `WindowGroup` still shows a Dock icon unless
  `LSUIElement` is set; no single page states this.
- For the design's monochrome microphone glyph, load an `NSImage`, set
  `isTemplate = true`, and wrap it in `Image(nsImage:)`. A `systemImage:` renders
  as a template automatically. Assembled from secondary sources:
  https://mirzoyan.dev/blog/custom-icon-menubarextra/ and
  https://developer.apple.com/forums/thread/738716
- The `.window` panel sizes itself only at first presentation, which the design's
  collapsible Input card must accommodate.
  https://github.com/mattt/iMCP/pull/189
- No source confirms or denies an `/Applications` location requirement for
  `SMAppService.mainApp.register()`.

## Question 11: one multiplatform target versus separate targets

### Verified from source

- The announcing session is WWDC22 110371, "Use Xcode to develop a multiplatform
  app". https://developer.apple.com/videos/play/wwdc2022/110371/
  Documentation page:
  https://developer.apple.com/documentation/xcode/configuring-a-multiplatform-app-target
- "Most settings in the target editor now come with a Conditions option", keyed
  on SDK; one target covers iOS, iPadOS, macOS and tvOS, watchOS stays separate,
  and shared capabilities are "combined into a single entitlements file".
  https://wwdcnotes.com/documentation/wwdc22-110371-use-xcode-to-develop-a-multiplatform-app/
- For genuinely per-platform entitlements, Apple's DTS engineer advises: "have two
  .entitlements files and then conditionalise the Code Signing Entitlements
  (CODE_SIGN_ENTITLEMENTS) build setting".
  https://developer.apple.com/forums/thread/727135
- **One bundle identifier.** "All destinations will use the same bundle
  identifier by default" (wwdcnotes URL above), and App Store Connect requires it
  when adding a macOS platform to an existing record: "you must set the bundle
  IDs to match the iOS app's bundle ID", sharing the same Apple ID, SKU and
  bundle ID.
  https://developer.apple.com/help/app-store-connect/create-an-app-record/add-platforms/
- SwiftPM's `platforms:` applies to the whole package; `target()` and
  `executableTarget()` take no platform parameter.
  https://docs.swift.org/package-manager/PackageDescription/PackageDescription.html
  https://forums.swift.org/t/platform-per-target/29253
  https://forums.swift.org/t/swiftpm-platform-specific-targets-products/31140
- Documented breakage cases for an executable target alongside an iOS app: a
  build-tool plugin depending on an `executableTarget` fails when the iOS app is
  archived (https://github.com/swiftlang/swift-package-manager/issues/6658), and
  `xcodebuild -sdk iphoneos` forces tool targets to build for iOS. An Apple DTS
  reply gives the fix: use `-destination generic/platform=iOS`.
  https://developer.apple.com/forums/thread/650302

### Inferred

- A single multiplatform target fits Calliope: the design already shares screens,
  and the platform differences (menu bar shell, device picker, background audio)
  are `#if os(...)` sized rather than target sized. Per-platform Info.plist and
  entitlements are handled by conditioned build settings such as
  `INFOPLIST_FILE[sdk=macosx*]`, which follows from the Conditions feature though
  no verbatim Apple sentence states it.
- The plain case here, where the iOS app links only the `MicCore` library product
  and the package also holds an unlinked `micsend` executable target, is not
  covered by any source found. Xcode most likely builds only referenced products,
  but this is unconfirmed and should be tested early.
- If it does break, the fallbacks in order are: wrap the executable's entry point
  in `#if os(macOS)`; move the CLI into a second local package depending on the
  first; or build the CLI with `swift build` outside Xcode. Always build with
  `-destination` rather than `-sdk`.
- One bundle identifier across both platforms means the design's single app
  record is achievable, and the identifier should be chosen before any App ID is
  created, because free-tier App IDs are rate limited (question 12).

## Question 12: running locally on a free Apple ID, and TestFlight later

The free path is the primary one. The paid programme is a later decision.

### Verified from source: free Apple ID as the primary path

- Free tier limits: 7-day provisioning profiles against one year for paid, up to
  3 devices, 10 App IDs, 3 apps per device.
  https://developer.apple.com/support/compare-memberships/
  The error text confirms the App ID limit: "You may create up to 10 App IDs
  every 7 days". https://developer.apple.com/forums/thread/675347
  Renewal means rebuilding from Xcode; there is no over-the-air route.
  https://developer.apple.com/forums/thread/761325
- **`UIBackgroundModes` = `audio` is an Info.plist key, not an entitlement, so it
  works on a Personal Team.** https://developer.apple.com/forums/thread/791736
- `NSLocalNetworkUsageDescription` and `NSBonjourServices` are likewise Info.plist
  keys and work on the free tier.
  https://developer.apple.com/documentation/bundleresources/information-property-list/nslocalnetworkusagedescription
  The separate `com.apple.developer.networking.multicast` entitlement does require
  an Apple request, but this design does not need it (question 7).
  https://developer.apple.com/forums/thread/666350
- Local notifications need no entitlement.
  https://developer.apple.com/forums/thread/66734
- **Keychain:** the `keychain-access-groups` capability (Keychain Sharing) fails
  on a Personal Team, but plain per-app Keychain with no access group works. The
  historical "all Keychain access broken" reports trace to an Xcode 8 beta bug.
  https://github.com/juliansteenbakker/flutter_secure_storage/issues/1176
  https://developer.apple.com/forums/message/352580
- Not supported on a Personal Team, none of which this design uses: Push
  Notifications ("Personal development teams … do not support the Push
  Notifications capability", https://developer.apple.com/forums/thread/718388),
  App Groups
  (https://mybyways.com/blog/new-limitations-imposed-on-free-apple-developer-account),
  and Sign in with Apple
  (https://developer.apple.com/forums/thread/701041).
- **macOS without a paid account.** "Sign to Run Locally" is ad-hoc signing
  (`CODE_SIGN_IDENTITY = -`). Apple's DTS engineer says it "can cause all sorts
  of weird issues", including with local network privacy, and recommends Apple
  Development signing through the free Personal Team instead.
  https://developer.apple.com/forums/thread/763141
  https://developer.apple.com/forums/thread/799910
- TCC keys a grant to the app's designated requirement, which for ad-hoc signing
  is the cdhash and changes on every build, so a microphone grant does not
  survive rebuilds; a stable Apple Development identity keeps it. Independent
  source, not Apple:
  https://eclecticlight.co/2025/11/08/explainer-permissions-privacy-and-tcc/
- A Developer ID Application certificate requires paid membership.
  https://developer.apple.com/help/account/certificates/create-developer-id-certificates/
- A locally built app carries no `com.apple.quarantine` attribute, so Gatekeeper
  is not triggered. https://developer.apple.com/forums/thread/706442

### Verified from source: TestFlight, if the paid programme is bought later

- Internal testers: up to 100 App Store Connect users, and internal testing does
  **not** require Beta App Review; external testing does. Builds expire after
  90 days.
  https://developer.apple.com/help/app-store-connect/test-a-beta-version/testflight-overview
- TestFlight supports macOS builds; no restriction on `LSUIElement` apps was
  found. https://developer.apple.com/videos/play/wwdc2021/10170/
- One App Store Connect record and one bundle ID cover both platforms.
  https://developer.apple.com/help/app-store-connect/create-an-app-record/add-platforms/
- The privacy manifest is enforced at upload (ITMS-91055), so it gates TestFlight
  uploads as well as App Store submissions.
  https://developer.apple.com/forums/thread/734748
- A missing purpose string is an upload rejection (ITMS-90683). Secondary source:
  https://medium.com/@paghadalsneh/itms-90683-missing-purpose-string-in-info-plist-or-nsphotolibraryusagedescription-53b8ed311579
- Export compliance is declared with `ITSAppUsesNonExemptEncryption`.
  https://developer.apple.com/documentation/bundleresources/information-property-list/itsappusesnonexemptencryption
  https://developer.apple.com/help/app-store-connect/reference/app-information/export-compliance-documentation-for-encryption/

### What the paid programme adds for this app

- One-year provisioning profiles instead of seven-day ones.
- More than three devices.
- TestFlight installs without a cable.
- Developer ID plus notarisation, so other people can run the Mac build.
- Keychain Sharing, App Groups and Push Notifications, none of which this design
  needs.

### Set the project up once so it works under both

- Choose the final reverse-DNS bundle identifier now. Free-tier App IDs count
  against the ten-per-seven-days limit, and the identifier must be the same
  across iOS and macOS.
- Use plain per-app Keychain with no access group. Do not add
  `keychain-access-groups` or App Groups; both would work under the paid
  programme only and would force a change later.
- Keep two entitlements files behind a conditioned `CODE_SIGN_ENTITLEMENTS`
  setting from the start.
- Sign the Mac build with the Personal Team's Apple Development identity rather
  than "Sign to Run Locally". That keeps the TCC microphone grant stable across
  rebuilds and avoids the background-execution problem noted in question 10.

### Inferred

- Whether `SMAppService.mainApp.register()` works under Apple Development
  (non-notarised) signing is unconfirmed; only the ad-hoc case is confirmed bad.
- Whether the seven-day expiry applies to locally signed macOS builds is
  unconfirmed.
- For a single user's Mac build, Developer ID plus `notarytool` is less work than
  TestFlight: no app record, no metadata, no privacy-manifest gate and no 90-day
  expiry. This is a reasoned conclusion from the sourced steps, not an Apple
  statement.
- The export-compliance exemption for an app using only AES-GCM and HTTPS was not
  settled. The exemption turns on whether only operating-system-provided
  cryptography is used, and must be checked against Apple's export compliance
  page before any upload rather than assumed.
- The required-reason API categories most likely to apply are `UserDefaults`, and
  possibly file timestamps and system boot time, depending on final code.
- A missing `NSLocalNetworkUsageDescription` is assumed to be an upload rejection
  by analogy with the microphone case; this was not directly sourced.

## Decisions for the plan

1. **Opus encoding.** Use `AVAudioConverter` with `kAudioFormatOpus`,
   `mFramesPerPacket = 960`, `bitRateStrategy` constant, `bitRate = 32000`. It
   emits exactly one raw 20 ms RFC 6716 packet per call, proven by decoding the
   output with stock libopus. Drop the libopus dependency from the design. Main
   source: local probe plus Apple's DTS example,
   https://developer.apple.com/forums/thread/763362
2. **Capture pipeline.** Install the tap with the input node's own hardware
   format and let one `AVAudioConverter` do rate conversion, channel reduction
   and re-chunking to 960 frames. Set `downmix = true`. No ring buffer.
   Main source: https://developer.apple.com/documentation/avfaudio/avaudioconverter
   and the `downmix` header text in `AVAudioConverter.h`.
3. **iOS audio session.** `.record` category, `.default` mode, **empty options**,
   then `setPreferredInput` to `.builtInMic` after activation. Omitting the
   Bluetooth option is documented to hide HFP devices entirely. Main source:
   https://developer.apple.com/documentation/avfaudio/avaudiosession/categoryoptions-swift.struct/allowbluetooth
4. **Voice processing.** Do not enable it. It requires an output node, which
   forces `.playAndRecord`, which implies `voiceChat` mode, which implies
   `allowBluetoothHFP`. This contradicts the approved design and the design
   should be amended. Main source:
   https://developer.apple.com/documentation/avfaudio/avaudiosession/mode-swift.struct/voicechat
5. **Background operation.** `UIBackgroundModes` = `audio` plus
   `NSMicrophoneUsageDescription`; start capture in the foreground, because an
   app cannot begin recording from the background. Handle interruptions by
   reactivating the session and restarting the engine, and handle
   `AVAudioEngineConfigurationChange` off its delivering queue. Main source:
   https://developer.apple.com/documentation/avfaudio/avaudiosession/category-swift.struct/record
6. **macOS device selection.** Set the device on `inputNode` before reading its
   format, and never touch `outputNode` or `mainMixerNode`, which is what drags
   AirPods into the hands-free profile. Detect Bluetooth with
   `kAudioDevicePropertyTransportType` matching `'blue'` or `'blea'`. Fall back to
   `AVCaptureSession` if the input-only engine proves fragile. Main sources:
   https://developer.apple.com/forums/thread/71008 and
   https://supermegaultragroovy.com/2021/01/28/more-on-avaudioengine-airpods/
7. **Networking.** One long-lived `NWConnection` over UDP with
   `serviceClass = .interactiveVoice`, `.idempotent` sends, and pongs received on
   the same connection via a self-re-arming `receiveMessage`. Pass the Bonjour
   `.service` endpoint straight to `NWConnection` rather than resolving. Main
   source: https://developer.apple.com/documentation/network/nwconnection/receivemessage(completion:)
8. **TLS pinning.** Implement `urlSession(_:didReceive:completionHandler:)`, take
   `SecTrustCopyCertificateChain(trust).first`, hash its `SecCertificateCopyData`
   with SHA-256, compare, and return `.useCredential`. Do **not** call
   `SecTrustEvaluateWithError`. Set `NSAllowsLocalNetworking` and connect by
   `.local` name, because iOS 17 and macOS 14 stopped allowing raw-IP HTTPS by
   default. Main sources: https://developer.apple.com/forums/thread/703234 and
   https://developer.apple.com/documentation/bundleresources/information-property-list/nsapptransportsecurity/nsallowslocalnetworking
9. **CryptoKit.** `AES.GCM.seal(_:using:nonce:)` with a 12-byte
   `AES.GCM.Nonce(data:)`, taking `.tag` and `.ciphertext` separately and never
   `.combined`; `SymmetricKey(data:)` of 16 bytes gives AES-128; `HMAC<SHA256>`
   accepts a 4-byte PIN key and matches RFC 2104. Main source:
   https://developer.apple.com/documentation/cryptokit/aes/gcm/sealedbox
10. **macOS shell.** `MenuBarExtra` with `.menuBarExtraStyle(.window)`,
    `LSUIElement` true, and `SMAppService.mainApp.register()` for the login item.
    Accept that the panel cannot be closed programmatically and that Settings
    needs an activation-policy workaround. Main source:
    https://developer.apple.com/documentation/swiftui/menubarextra
11. **Targets.** One multiplatform app target with conditioned build settings, one
    bundle identifier, two entitlements files behind a conditioned
    `CODE_SIGN_ENTITLEMENTS`. Keep `MicCore` and `micsend` in one local package
    and always build with `-destination`, never `-sdk`. Main source:
    https://developer.apple.com/forums/thread/650302
12. **Provisioning.** Start on a free Personal Team: the background audio mode,
    Bonjour, local network access, local notifications and plain per-app Keychain
    all work; only `keychain-access-groups`, App Groups and Push do not, and the
    design needs none of them. Sign the Mac build with the Personal Team's Apple
    Development identity rather than "Sign to Run Locally", so the TCC microphone
    grant survives rebuilds. Main sources:
    https://developer.apple.com/support/compare-memberships/ and
    https://developer.apple.com/forums/thread/799910

## Still unverified, needs a device test

1. **Does the iOS Opus encoder exist on a real iPhone?** Run
   `AudioFormatGetPropertyInfo`/`AudioFormatGetProperty` for
   `kAudioFormatProperty_EncodeFormatIDs` on device and assert the returned array
   contains `kAudioFormatOpus`. On this Mac it returned 16 ids including Opus.
   If it fails, `AVAudioConverter(from:to:)` returns nil and the libopus fallback
   is needed. This is a one-line check and should be the first thing milestone 3
   runs.
2. **Does a `.record` session with empty options pull AirPods off the Apple TV?**
   Connect AirPods to the Apple TV, play audio, then start Calliope on the iPhone
   and activate the session. Confirm the Apple TV audio does not drop out, that
   `availableInputs` contains no Bluetooth port, and that `currentRoute.inputs`
   is the built-in microphone. This is the highest-risk assumption in the design
   and no documentation settles it.
3. **What sample rate does the iPhone actually deliver?** After activation, log
   `AVAudioSession.sharedInstance().sampleRate` and
   `engine.inputNode.outputFormat(forBus: 0)`. Do not hardcode 48000; confirm the
   converter chain handles whatever arrives.
4. **Does capture survive the screen locking, and for how long?** Start a mic
   session, lock the phone, and confirm packets keep arriving at the host for at
   least ten minutes. Then take an incoming phone call and confirm the
   interruption began and ended path restores capture without a foreground tap.
5. **Does AVAudioEngine start on a macOS input-only device when only `inputNode`
   is used?** Select "MacBook Pro Microphone" by device id, never reference
   `outputNode` or `mainMixerNode`, and start the engine. Confirm it starts and
   that AirPods connected as output stay in A2DP, by checking their output
   sample rate is still 48 kHz rather than dropping to 16 kHz.
6. **Does the iOS local network prompt appear and can denial be detected?** Run
   on a device, not the Simulator, which does not implement local network
   privacy. Confirm the prompt appears on first `NWBrowser` start, then deny it
   and confirm the browser reports `.waiting(.dns(-65570))`.
7. **Does HTTPS to the host succeed on iOS 17+?** Test both a `.local` hostname
   and a raw LAN IP address with `NSAllowsLocalNetworking` set, and record which
   works. If the raw IP fails with -1200, the private CIDR ranges must be added
   under `NSExceptionDomains`.
8. **Does an unlinked `micsend` executable target break the iOS app build?**
   Archive the iOS app with the package present and confirm the executable target
   is not built for iOS.
9. **Does `SMAppService.mainApp.register()` succeed under Apple Development
   signing?** Register the login item, restart the Mac, and confirm the app
   relaunches. Only the ad-hoc signing case is confirmed to fail.
