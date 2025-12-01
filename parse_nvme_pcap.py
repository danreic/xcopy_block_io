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
    # Bytes 4-7: PDU Length (24-bit) + reserved
    
    pdu_type = data[0]
    flags = data[1]
    pdu_length = struct.unpack('<I', data[4:8])[0] & 0xFFFFFF
    
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
    
    for pkt in packets:
        if TCP in pkt and Raw in pkt:
            tcp = pkt[TCP]
            raw = pkt[Raw]
            data = bytes(raw.load)
            
            # Look for NVMe-TCP PDUs
            offset = 0
            while offset < len(data):
                pdu = parse_nvme_tcp_pdu(data[offset:])
                if not pdu:
                    break
                
                # PDU Type 0x00 = NVMe command
                if pdu['pdu_type'] == 0x00:
                    cmd = parse_nvme_command(pdu['data'])
                    if cmd and cmd['opcode'] == 0x19:  # XCOPY
                        xcopy_commands.append({
                            'packet': pkt,
                            'pdu': pdu,
                            'command': cmd,
                            'data': pdu['data'][64:] if len(pdu['data']) > 64 else b''
                        })
                
                offset += 8 + pdu['pdu_length']
            
            # Also collect data packets for analysis
            if len(data) > 64:
                data_packets.append(data)
    
    print(f"Found {len(xcopy_commands)} XCOPY commands")
    
    if xcopy_commands:
        for i, xcopy in enumerate(xcopy_commands):
            cmd = xcopy['command']
            print(f"\n--- XCOPY Command {i+1} ---")
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
            
            print(f"\nCommand Hex (first 64 bytes):")
            print(f"  {cmd['raw'].hex()}")
    
    return xcopy_commands

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

