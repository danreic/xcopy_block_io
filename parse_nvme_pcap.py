#!/usr/bin/env python3
"""
Parse NVMe-TCP pcap files to extract and compare XCOPY commands
"""

import sys
import struct
from scapy.all import rdpcap, TCP, Raw

def parse_nvme_tcp_pdu(data):
    """Parse NVMe-TCP PDU header"""
    if len(data) < 8:
        return None
    
    # NVMe-TCP PDU header (8 bytes)
    # Byte 0: PDU Type
    # Byte 1: Flags
    # Bytes 2-3: Header Digest (if enabled)
    # Bytes 4-6: PDU Length (24-bit, little-endian)
    # Byte 7: Reserved
    
    pdu_type = data[0]
    flags = data[1]
    # PDU length is 24-bit in bytes 4-6 (little-endian)
    pdu_length = data[4] | (data[5] << 8) | (data[6] << 16)
    
    return {
        'pdu_type': pdu_type,
        'flags': flags,
        'pdu_length': pdu_length,
        'data': data[8:] if len(data) > 8 else b''
    }

def parse_nvme_command(data):
    """Parse NVMe command structure (64 bytes)"""
    if len(data) < 64:
        return None
    
    # NVMe command structure
    opcode = data[0]
    flags = data[1]
    command_id = struct.unpack('<H', data[2:4])[0]
    nsid = struct.unpack('<I', data[4:8])[0]
    cdw2 = struct.unpack('<I', data[8:12])[0]
    cdw3 = struct.unpack('<I', data[12:16])[0]
    cdw10 = struct.unpack('<I', data[16:20])[0]
    cdw11 = struct.unpack('<I', data[20:24])[0]
    cdw12 = struct.unpack('<I', data[24:28])[0]
    cdw13 = struct.unpack('<I', data[28:32])[0]
    cdw14 = struct.unpack('<I', data[32:36])[0]
    cdw15 = struct.unpack('<I', data[36:40])[0]
    
    return {
        'opcode': opcode,
        'flags': flags,
        'command_id': command_id,
        'nsid': nsid,
        'cdw10': cdw10,
        'cdw11': cdw11,
        'cdw12': cdw12,
        'cdw13': cdw13,
        'cdw14': cdw14,
        'cdw15': cdw15,
        'raw': data[:64]
    }

def parse_xcopy_range_descriptor(data):
    """Parse XCOPY range descriptor (32 bytes)"""
    if len(data) < 32:
        return None
    
    rsvd0 = struct.unpack('<I', data[0:4])[0]
    src_nsid = struct.unpack('<I', data[4:8])[0]
    src_lba = struct.unpack('<Q', data[8:16])[0]
    rsvd1 = struct.unpack('<I', data[16:20])[0]
    num_blocks = struct.unpack('<I', data[20:24])[0]
    dst_lba = struct.unpack('<Q', data[24:32])[0]
    
    return {
        'rsvd0': rsvd0,
        'src_nsid': src_nsid,
        'src_lba': src_lba,
        'rsvd1': rsvd1,
        'num_blocks': num_blocks,
        'dst_lba': dst_lba,
        'raw': data[:32]
    }

def analyze_pcap(filename):
    """Analyze pcap file for NVMe-TCP XCOPY commands"""
    print(f"\n=== Analyzing {filename} ===")
    
    try:
        packets = rdpcap(filename)
    except Exception as e:
        print(f"Error reading pcap: {e}")
        return None
    
    print(f"Total packets: {len(packets)}")
    
    xcopy_commands = []
    data_packets = []
    all_pdu_types = set()
    
    # First pass: collect all TCP payloads and look for XCOPY opcode directly
    # Also track TCP streams for reassembly
    tcp_streams = {}  # (src_ip, src_port, dst_ip, dst_port) -> list of payloads
    
    for pkt in packets:
        if TCP in pkt and Raw in pkt:
            tcp = pkt[TCP]
            raw = pkt[Raw]
            data = bytes(raw.load)
            
            # Track TCP stream
            if 'IP' in pkt:
                ip = pkt['IP']
                stream_key = (ip.src, tcp.sport, ip.dst, tcp.dport)
                if stream_key not in tcp_streams:
                    tcp_streams[stream_key] = []
                tcp_streams[stream_key].append(data)
            
            # Look for XCOPY opcode (0x19) directly in the payload
            # This helps us find commands even if PDU parsing fails
            for i in range(len(data) - 64):
                if data[i] == 0x19:  # XCOPY opcode
                    # Try to parse as NVMe command
                    cmd = parse_nvme_command(data[i:])
                    if cmd and cmd['opcode'] == 0x19 and cmd['nsid'] > 0:
                        # Found potential XCOPY command
                        # Check if we have range descriptor data
                        range_data = data[i+64:i+64+32] if len(data) >= i+64+32 else b''
                        xcopy_commands.append({
                            'packet': pkt,
                            'offset': i,
                            'command': cmd,
                            'data': range_data,
                            'raw_payload': data[i:i+64+32]
                        })
            
            # Also try to parse NVMe-TCP PDUs
            offset = 0
            while offset < len(data) - 8:
                pdu = parse_nvme_tcp_pdu(data[offset:])
                if not pdu:
                    break
                
                all_pdu_types.add(pdu['pdu_type'])
                
                # PDU Type 0x00 = NVMe command, 0x04 = I/O command
                # For I/O commands (type 4), the NVMe command starts after the PDU header
                # For type 4, there's an additional 8-byte I/O command header before the NVMe command
                if pdu['pdu_type'] in [0x00, 0x04]:
                    # For PDU type 4 (I/O command), skip 8-byte I/O command header
                    cmd_offset = 0
                    if pdu['pdu_type'] == 0x04:
                        cmd_offset = 8  # I/O command header is 8 bytes
                    
                    if len(pdu['data']) >= cmd_offset + 64:
                        cmd_data = pdu['data'][cmd_offset:]
                        cmd = parse_nvme_command(cmd_data)
                        if cmd and cmd['opcode'] == 0x19:  # XCOPY
                            # Check if we already found this one
                            found = False
                            for existing in xcopy_commands:
                                if existing['command']['nsid'] == cmd['nsid'] and \
                                   existing['command']['cdw10'] == cmd['cdw10']:
                                    found = True
                                    break
                            if not found:
                                range_data = cmd_data[64:] if len(cmd_data) > 64 else b''
                                xcopy_commands.append({
                                    'packet': pkt,
                                    'pdu': pdu,
                                    'command': cmd,
                                    'data': range_data
                                })
                
                # Move to next PDU
                next_offset = offset + 8 + pdu['pdu_length']
                if next_offset > len(data) or next_offset <= offset:
                    break
                offset = next_offset
            
            # Also collect data packets for analysis
            if len(data) > 64:
                data_packets.append(data)
    
    print(f"Found PDU types: {sorted(all_pdu_types)}")
    print(f"Found {len(xcopy_commands)} XCOPY commands (before deduplication)")
    
    # Remove duplicates based on command signature
    unique_commands = []
    if xcopy_commands:
        seen = set()
        for xcopy in xcopy_commands:
            cmd = xcopy['command']
            sig = (cmd['opcode'], cmd['nsid'], cmd['cdw10'], cmd['cdw11'])
            if sig not in seen:
                seen.add(sig)
                unique_commands.append(xcopy)
    
    print(f"Unique XCOPY commands: {len(unique_commands)}")
    
    if unique_commands:
        for i, xcopy in enumerate(unique_commands):
            cmd = xcopy['command']
            print(f"\n--- XCOPY Command {i+1} ---")
            if 'offset' in xcopy:
                print(f"Found at offset {xcopy['offset']} in packet")
            if 'pdu' in xcopy:
                print(f"PDU Type: 0x{xcopy['pdu']['pdu_type']:02x}, Length: {xcopy['pdu']['pdu_length']}")
            print(f"Opcode: 0x{cmd['opcode']:02x} (XCOPY)")
            print(f"NSID: {cmd['nsid']}")
            print(f"CDW10: 0x{cmd['cdw10']:08x} (num_ranges-1)")
            print(f"CDW11-15: 0x{cmd['cdw11']:08x} 0x{cmd['cdw12']:08x} 0x{cmd['cdw13']:08x} 0x{cmd['cdw14']:08x} 0x{cmd['cdw15']:08x}")
            
            # Parse range descriptors
            data = xcopy['data']
            if len(data) >= 32:
                range_desc = parse_xcopy_range_descriptor(data[:32])
                if range_desc:
                    print(f"\nRange Descriptor:")
                    print(f"  src_nsid: {range_desc['src_nsid']}")
                    print(f"  src_lba: {range_desc['src_lba']}")
                    print(f"  num_blocks: {range_desc['num_blocks']} (0-based, actual={range_desc['num_blocks']+1})")
                    print(f"  dst_lba: {range_desc['dst_lba']}")
                    print(f"\nRange Descriptor Hex:")
                    print(f"  {range_desc['raw'].hex()}")
            else:
                print(f"\nWARNING: Range descriptor data too short ({len(data)} bytes, need 32)")
            
            print(f"\nCommand Hex (first 64 bytes):")
            print(f"  {cmd['raw'].hex()}")
            
            if 'raw_payload' in xcopy:
                print(f"\nFull payload hex (command + range descriptor):")
                hex_str = xcopy['raw_payload'].hex()
                for j in range(0, len(hex_str), 32):
                    print(f"  {hex_str[j:j+32]}")
    
    # Remove duplicates based on command signature
    unique_commands = []
    if xcopy_commands:
        seen = set()
        for xcopy in xcopy_commands:
            cmd = xcopy['command']
            sig = (cmd['opcode'], cmd['nsid'], cmd['cdw10'], cmd['cdw11'])
            if sig not in seen:
                seen.add(sig)
                unique_commands.append(xcopy)
    
    # If no commands found, search all TCP streams for XCOPY commands
    # Parse PDUs properly in the stream
    if not unique_commands:
        print("\nNo XCOPY commands found in individual packets. Parsing TCP streams for PDUs...")
        print(f"Found {len(tcp_streams)} TCP streams")
        for stream_key, stream_data in tcp_streams.items():
            # Reassemble stream
            stream_payload = b''.join(stream_data)
            print(f"  Stream {stream_key}: {len(stream_payload)} bytes total")
            offset = 0
            pdu_count = 0
            
            while offset < len(stream_payload) - 8:
                # Try to parse PDU
                pdu = parse_nvme_tcp_pdu(stream_payload[offset:])
                if not pdu or pdu['pdu_length'] == 0:
                    # Not a valid PDU, try next byte
                    offset += 1
                    continue
                
                pdu_count += 1
                if pdu_count <= 5:  # Debug first few PDUs
                    print(f"    PDU {pdu_count} at offset {offset}: type=0x{pdu['pdu_type']:02x}, length={pdu['pdu_length']}")
                
                # For PDU type 0x00 (NVMe command) and 0x04 (I/O command)
                if pdu['pdu_type'] == 0x00:  # NVMe command PDU
                    # NVMe command PDU structure:
                    # - 8 bytes: PDU header (already parsed)
                    # - 64 bytes: NVMe command
                    # - N bytes: Data (range descriptors for XCOPY)
                    
                    nvme_cmd_offset = offset + 8
                    
                    if nvme_cmd_offset + 64 <= len(stream_payload):
                        cmd = parse_nvme_command(stream_payload[nvme_cmd_offset:])
                        if cmd and cmd['opcode'] == 0x19:  # XCOPY
                            print(f"\nFound XCOPY command in PDU type 0x00 at offset {offset} in stream {stream_key}")
                            print(f"  PDU type: 0x{pdu['pdu_type']:02x}, length: {pdu['pdu_length']}")
                            print(f"  NVMe command at offset: {nvme_cmd_offset}")
                            print(f"  Command hex: {cmd['raw'].hex()}")
                            print(f"  NSID: {cmd['nsid']}, CDW10: 0x{cmd['cdw10']:08x}")
                            
                            # Get range descriptor data (after NVMe command)
                            data_offset = nvme_cmd_offset + 64
                            range_data = stream_payload[data_offset:data_offset+32] if len(stream_payload) >= data_offset+32 else b''
                            
                            if len(range_data) >= 32:
                                range_desc = parse_xcopy_range_descriptor(range_data)
                                if range_desc:
                                    print(f"  Range: src_nsid={range_desc['src_nsid']}, src_lba={range_desc['src_lba']}, dst_lba={range_desc['dst_lba']}, num_blocks={range_desc['num_blocks']}")
                                    print(f"  Range hex: {range_desc['raw'].hex()}")
                            
                            unique_commands.append({
                                'command': cmd,
                                'data': range_data,
                                'stream': stream_key,
                                'offset': offset,
                                'pdu': pdu
                            })
                        elif pdu_count <= 5 and nvme_cmd_offset + 64 <= len(stream_payload):
                            # Debug: show first few commands in PDU type 0x00
                            cmd = parse_nvme_command(stream_payload[nvme_cmd_offset:])
                            if cmd:
                                print(f"    PDU 0x00 at offset {offset}: opcode=0x{cmd['opcode']:02x}, nsid={cmd['nsid']}")
                
                elif pdu['pdu_type'] == 0x04:  # I/O command
                    # I/O command PDU structure:
                    # - 8 bytes: PDU header (already parsed)
                    # - 8 bytes: I/O command capsule header
                    # - 64 bytes: NVMe command
                    # - N bytes: Data (range descriptors for XCOPY)
                    
                    io_header_offset = offset + 8
                    nvme_cmd_offset = io_header_offset + 8
                    
                    if nvme_cmd_offset + 64 <= len(stream_payload):
                        cmd = parse_nvme_command(stream_payload[nvme_cmd_offset:])
                        if cmd and cmd['opcode'] == 0x19:  # XCOPY
                            print(f"\nFound XCOPY command in PDU type 0x04 at offset {offset} in stream {stream_key}")
                            print(f"  PDU type: 0x{pdu['pdu_type']:02x}, length: {pdu['pdu_length']}")
                            print(f"  NVMe command at offset: {nvme_cmd_offset}")
                            print(f"  NSID: {cmd['nsid']}, CDW10: 0x{cmd['cdw10']:08x}")
                            
                            # Get range descriptor data (after NVMe command)
                            data_offset = nvme_cmd_offset + 64
                            range_data = stream_payload[data_offset:data_offset+32] if len(stream_payload) >= data_offset+32 else b''
                            
                            if len(range_data) >= 32:
                                range_desc = parse_xcopy_range_descriptor(range_data)
                                if range_desc:
                                    print(f"  Range: src_nsid={range_desc['src_nsid']}, src_lba={range_desc['src_lba']}, dst_lba={range_desc['dst_lba']}, num_blocks={range_desc['num_blocks']}")
                                    print(f"  Range hex: {range_desc['raw'].hex()}")
                            
                            unique_commands.append({
                                'command': cmd,
                                'data': range_data,
                                'stream': stream_key,
                                'offset': offset,
                                'pdu': pdu
                            })
                
                # Move to next PDU
                next_offset = offset + 8 + pdu['pdu_length']
                if next_offset > len(stream_payload) or next_offset <= offset:
                    break
                offset = next_offset
            
            print(f"    Parsed {pdu_count} PDUs in stream")
            
            # Also do a simple search for 0x19 as fallback (search entire stream)
            print(f"    Searching for 0x19 opcode in stream...")
            found_0x19_count = 0
            for i in range(len(stream_payload) - 64):
                if stream_payload[i] == 0x19:
                    found_0x19_count += 1
                    # Check if this looks like a valid NVMe command start
                    # (opcode 0x19, followed by reasonable values)
                    if i + 64 <= len(stream_payload):
                        cmd = parse_nvme_command(stream_payload[i:])
                    if cmd and cmd['opcode'] == 0x19:
                        print(f"    Found 0x19 at offset {i}, NSID={cmd['nsid']}, CDW10=0x{cmd['cdw10']:08x}")
                        # Show context to understand why NSID might be 0
                        if cmd['nsid'] == 0:
                            print(f"      WARNING: NSID is 0! Command hex: {cmd['raw'].hex()}")
                            print(f"      Context (offset {i-8} to {i+80}): {stream_payload[max(0,i-8):min(len(stream_payload),i+80)].hex()}")
                        # Check if we already found this one
                        found = False
                        for existing in unique_commands:
                            if 'command' in existing and existing['command']['nsid'] == cmd['nsid'] and \
                               existing['command']['cdw10'] == cmd['cdw10']:
                                found = True
                                break
                        if not found:
                            print(f"      Adding XCOPY command at offset {i}")
                            range_data = stream_payload[i+64:i+64+32] if len(stream_payload) >= i+64+32 else b''
                            if len(range_data) >= 32:
                                range_desc = parse_xcopy_range_descriptor(range_data)
                                if range_desc:
                                    print(f"      Range: src_nsid={range_desc['src_nsid']}, src_lba={range_desc['src_lba']}, dst_lba={range_desc['dst_lba']}, num_blocks={range_desc['num_blocks']}")
                            unique_commands.append({
                                'command': cmd,
                                'data': range_data,
                                'stream': stream_key,
                                'offset': i
                            })
            if found_0x19_count > 0:
                print(f"    Found {found_0x19_count} occurrences of 0x19 in stream")
    
    # If still no commands found, show some debug info
    if not unique_commands and data_packets:
        print("\nNo XCOPY commands found. Showing sample packet data:")
        sample = data_packets[0]
        print(f"First packet payload length: {len(sample)} bytes")
        print(f"First 64 bytes hex: {sample[:64].hex()}")
        # Look for any opcode 0x19
        for i, byte in enumerate(sample[:100]):
            if byte == 0x19:
                print(f"Found 0x19 at offset {i}")
                if i + 64 <= len(sample):
                    print(f"  Context: {sample[max(0,i-4):i+64].hex()}")
    
    return unique_commands if unique_commands else []

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 parse_nvme_pcap.py <pcap_file1> [pcap_file2]")
        print("  If two files are provided, they will be compared")
        sys.exit(1)
    
    file1_commands = analyze_pcap(sys.argv[1])
    
    if len(sys.argv) >= 3:
        file2_commands = analyze_pcap(sys.argv[2])
        
        # Compare
        print("\n=== Comparison ===")
        if file1_commands and file2_commands:
            if len(file1_commands) > 0 and len(file2_commands) > 0:
                cmd1 = file1_commands[0]['command']
                cmd2 = file2_commands[0]['command']
                
                print("\nCommand Structure Comparison:")
                print(f"  Opcode: {cmd1['opcode']:02x} vs {cmd2['opcode']:02x} {'✓' if cmd1['opcode'] == cmd2['opcode'] else '✗'}")
                print(f"  NSID: {cmd1['nsid']} vs {cmd2['nsid']} {'✓' if cmd1['nsid'] == cmd2['nsid'] else '✗'}")
                print(f"  CDW10: {cmd1['cdw10']:08x} vs {cmd2['cdw10']:08x} {'✓' if cmd1['cdw10'] == cmd2['cdw10'] else '✗'}")
                
                if len(file1_commands[0]['data']) >= 32 and len(file2_commands[0]['data']) >= 32:
                    range1 = parse_xcopy_range_descriptor(file1_commands[0]['data'][:32])
                    range2 = parse_xcopy_range_descriptor(file2_commands[0]['data'][:32])
                    if range1 and range2:
                        print("\nRange Descriptor Comparison:")
                        print(f"  src_nsid: {range1['src_nsid']} vs {range2['src_nsid']} {'✓' if range1['src_nsid'] == range2['src_nsid'] else '✗'}")
                        print(f"  src_lba: {range1['src_lba']} vs {range2['src_lba']} {'✓' if range1['src_lba'] == range2['src_lba'] else '✗'}")
                        print(f"  num_blocks: {range1['num_blocks']} vs {range2['num_blocks']} {'✓' if range1['num_blocks'] == range2['num_blocks'] else '✗'}")
                        print(f"  dst_lba: {range1['dst_lba']} vs {range2['dst_lba']} {'✓' if range1['dst_lba'] == range2['dst_lba'] else '✗'}")
                        
                        if range1['raw'] != range2['raw']:
                            print("\n⚠️  Range descriptors differ!")
                            print(f"  File1: {range1['raw'].hex()}")
                            print(f"  File2: {range2['raw'].hex()}")
                        else:
                            print("\n✓ Range descriptors match!")

if __name__ == '__main__':
    main()

