iemcan - accessing the CANbus from within Pd
============================================

The Controller Area Network (CAN bus) is used in automotive and other
industries for inter-microcontroller communication.
This library allows to interact with the CAN bus from within Pd.

It was developed at the Institute of Electronic Music and Acoustics (iem) at
the University of Music and Performing Arts Graz (KUG), by IOhannes m zmölnig.

© 2018 IOhannes m zmölnig, IEM

# functionality

## receiving CAN data

the `[CANreceive]` object receives raw data from the CAN bus.
The data is output as a list of bytes (similar to `[netreceive -b]`),
with the selector being set to the CAN-id (as a hexadecimal representation, e.g.
"0x109").

## decoding CAN data
To extract parameters from the raw CAN data, use `[CANparse]`.
It will extract all data from a given CAN frame according to a signal
specification you have to pass to the object beforehand.

~~~
signal <name:s> <can_id:s> <start:i> <length:i> <big?:b> <signed?:b> [<factor:f> <offset:f>]
~~~

- `name` an arbitrary symbol that is used as a selector for outputting this parameter
- `can_id` a symbol identifying the sender of the CAN frame (`[CANreceive]` uses
  this symbol as selector for received messages in a hexadecimal representation,
  e.g. "0x107"
- `start` parameter position (in the frame) in bits
- `length` parameter size (in the frame) in bits
- `big?` whether the data is big-endian (1) or little-endian (0)
- `signed?` whether the data is signed or unsigned
- `factor` scaling factor (optional, default:1)
- `offset` offset (optional, default:0)

### using dbc as a parameter description:
`.dbc` is a proprietary file format by "Vector Informatik" to describe the
properties of a CAN network.
It contains information on how to extract a given parameter from a CAN frame.

E.g. signal definitions in the dbc file look like:
The relevant sections in the dbc file are the *Message Definitions* (`BO_`)
and the *Signal Definitions* (`SG_`), which are defined like this:

~~~
BO_ <CAN-ID> <MessageName>: <MessageLength> <SendingNode>

SG_ <SignalName> [M|m<MultiplexerIdentifier>] : <StartBit>|<Length>@<Endianness><Signed> (<Factor>,<Offset>) [<Min>|<Max>] "[Unit]" [ReceivingNodes]
~~~

Note these parameters:

- `CAN-ID` the CAN id as a number (convert this to a hex-string to obtain the
  `can_id` value)
- `StartBit` parameter position in bits (`start`)
- `Length` parameter size in bits (`length`)
- `Endianness` indicates *little-endian* (1) or *big-endian* (0)
- `Signed` either `+` (unsigned) or `-` (signed); Pd wants `0` (unsigned) or `1` (signed)
- `Factor` scaling factor `factor`
- `Offset` the `offset`

For example, consider the following definition:
~~~
BO_ 500 IO_DEBUG: 4 IO
 SG_ IO_DEBUG_test_unsigned : 0|8@1+ (1,0) [0|0] "" DBG
~~~

This translates to the following `signal` definition for `[CANparse`]:

~~~
signal test 0x1f4 0 8 1 + 1 0
~~~

- The `test` label is arbitrary, I could have used `ID_DEBUG_test_unsigned` like
  in the dbc file.
- The can-id `0x1f4` is *500* (as found in the `BO_` section) in hex.
- The remaining args are taken directly from the`SG_` section in the dbc file.

# supported devices
Currently iemcan uses the "socketcan" infrastructure to access the CAN bus.
Therefore, all devices supported by "socketcan" are supported.

Update 28.9.2026:
Systec USB-CANmodul1 is now supported under Windows.
