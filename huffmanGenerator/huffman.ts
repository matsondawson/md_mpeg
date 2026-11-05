import { Uint8Buffer } from "./buffer";

// Numbers are the values Frame.cpp's tables hold. Strings are "run,level" pairs or special codes.
export type VlcValue = number | string;

export type VlcRow = {
    value: VlcValue;
    length: number;
    code: string;
};

export class VlcTable {
    maxBitLength: number;
    decodingTable: Array<VlcRow | undefined>;

    constructor(public readonly name: string, codes: Record<string, VlcValue>) {
        this.maxBitLength = Object.keys(codes).reduce((maxLength: number, code: string): number =>
            Math.max(maxLength, code.replace(/ /g, "").length), 0);
        this.decodingTable = new Array<VlcRow | undefined>(Math.pow(2, this.maxBitLength)).fill(undefined);

        if (this.maxBitLength > 16) throw Error("Bit length too long for naive VLC implementation");

        for (let code in codes) {
            const value = codes[code];
            code = code.replace(/ /g, "");
            const start = parseInt(code.padEnd(this.maxBitLength, "0"), 2);
            const end = parseInt(code.padEnd(this.maxBitLength, "1"), 2);
            const row = { code, value, length: code.length };
            for (let i = start; i <= end; i++) {
                if (this.decodingTable[i]) throw Error(`${name}: ${code} overlaps ${this.decodingTable[i]!.code}`);
                this.decodingTable[i] = row;
            }
        }
    }

    // Prints one line per code: the decoding table indexes it fills, then its code, length and value
    print(): void {
        const unused = this.decodingTable.filter((row) => row === undefined).length;
        console.log(`\n${this.name}: ${this.maxBitLength} bit index, ${this.decodingTable.length} entries, ${unused} unused`);
        const width = Math.ceil(this.maxBitLength / 4);
        const hex = (n: number) => n.toString(16).padStart(width, "0");
        for (let start = 0; start < this.decodingTable.length;) {
            const row = this.decodingTable[start];
            let end = start;
            while (end + 1 < this.decodingTable.length && this.decodingTable[end + 1] === row) end++;
            if (row) {
                const range = `${hex(start)}-${hex(end)}`;
                console.log(`  ${range}  ${row.code.padEnd(this.maxBitLength)}  ${String(row.length).padStart(2)}  ${row.value}`);
            }
            start = end + 1;
        }
    }

    decode(buffer: Uint8Buffer): VlcRow | undefined {
        const v = buffer.peek(this.maxBitLength);
        const r = this.decodingTable[v];
        r && buffer.skip(r.length);
        return r;
    }

    public toString(): string {
        console.log(this);
        return `maxBitLength: ${this.maxBitLength}\ndecodingTable: ${this.decodingTable}`;
    }
}

// Spec table 2-B.1. 34 and 35 are what Frame.cpp uses for stuffing and escape.
export const macroblockAddressIncrementCodes = {
    "1": 1,
    "011": 2,
    "010": 3,
    "0011": 4,
    "0010": 5,
    "0001 1": 6,
    "0001 0": 7,
    "0000 111": 8,
    "0000 110": 9,
    "0000 1011": 10,
    "0000 1010": 11,
    "0000 1001": 12,
    "0000 1000": 13,
    "0000 0111": 14,
    "0000 0110": 15,
    "0000 0101 11": 16,
    "0000 0101 10": 17,
    "0000 0101 01": 18,
    "0000 0101 00": 19,
    "0000 0100 11": 20,
    "0000 0100 10": 21,
    "0000 0100 011": 22,
    "0000 0100 010": 23,
    "0000 0100 001": 24,
    "0000 0100 000": 25,
    "0000 0011 111": 26,
    "0000 0011 110": 27,
    "0000 0011 101": 28,
    "0000 0011 100": 29,
    "0000 0011 011": 30,
    "0000 0011 010": 31,
    "0000 0011 001": 32,
    "0000 0011 000": 33,
    "0000 0001 111": 34,
    "0000 0001 000": 35
};

// Spec table 2-B.2. Bit 4 macroblock_quant, 3 forward, 2 backward, 1 pattern, 0 intra.
export const macroblockTypeICodes = {
    "1": 0x01,
    "01": 0x11
};

export const macroblockTypePCodes = {
    "1": 0x0a,
    "01": 0x02,
    "001": 0x08,
    "0001 1": 0x01,
    "0001 0": 0x1a,
    "0000 1": 0x12,
    "0000 01": 0x11
};

export const macroblockTypeBCodes = {
    "10": 0x0c,
    "11": 0x0e,
    "010": 0x04,
    "011": 0x06,
    "0010": 0x08,
    "0011": 0x0a,
    "0001 1": 0x01,
    "0001 0": 0x1e,
    "0000 11": 0x1a,
    "0000 10": 0x16,
    "0000 01": 0x11
};

export const macroblockTypeDCodes = {
    "1": 0x01
};

// Spec table 2-B.3. Values are the coded block pattern.
const macroblockPattternCodes = {
    "111": 60,
    "1101": 4,
    "1100": 8,
    "1011": 16,
    "1010": 32,
    "1001 1": 12,
    "1001 0": 48,
    "1000 1": 20,
    "1000 0": 40,
    "0111 1": 28,
    "0111 0": 44,
    "0110 1": 52,
    "0110 0": 56,
    "0101 1": 1,
    "0101 0": 61,
    "0100 1": 2,
    "0100 0": 62,
    "0011 11": 24,
    "0011 10": 36,
    "0011 01": 3,
    "0011 00": 63,
    "0010 111": 5,
    "0010 110": 9,
    "0010 101": 17,
    "0010 100": 33,
    "0010 011": 6,
    "0010 010": 10,
    "0010 001": 18,
    "0010 000": 34,
    "0001 1111": 7,
    "0001 1110": 11,
    "0001 1101": 19,
    "0001 1100": 35,
    "0001 1011": 13,
    "0001 1010": 49,
    "0001 1001": 21,
    "0001 1000": 41,
    "0001 0111": 14,
    "0001 0110": 50,
    "0001 0101": 22,
    "0001 0100": 42,
    "0001 0011": 15,
    "0001 0010": 51,
    "0001 0001": 23,
    "0001 0000": 43,
    "0000 1111": 25,
    "0000 1110": 37,
    "0000 1101": 26,
    "0000 1100": 38,
    "0000 1011": 29,
    "0000 1010": 45,
    "0000 1001": 53,
    "0000 1000": 57,
    "0000 0111": 30,
    "0000 0110": 46,
    "0000 0101": 54,
    "0000 0100": 58,
    "0000 0011 1": 31,
    "0000 0011 0": 47,
    "0000 0010 1": 55,
    "0000 0010 0": 59,
    "0000 0001 1": 27,
    "0000 0001 0": 39
}

// Spec table 2-B.4
export const motionCodes = {
    "0000 0011 001": -16,
    "0000 0011 011": -15,
    "0000 0011 101": -14,
    "0000 0011 111": -13,
    "0000 0100 001": -12,
    "0000 0100 011": -11,
    "0000 0100 11": -10,
    "0000 0101 01": -9,
    "0000 0101 11": -8,
    "0000 0111": -7,
    "0000 1001": -6,
    "0000 1011": -5,
    "0000 111": -4,
    "0001 1": -3,
    "0011": -2,
    "011": -1,
    "1": 0,
    "010": 1,
    "0010": 2,
    "0001 0": 3,
    "0000 110": 4,
    "0000 1010": 5,
    "0000 1000": 6,
    "0000 0110": 7,
    "0000 0101 10": 8,
    "0000 0101 00": 9,
    "0000 0100 10": 10,
    "0000 0100 010": 11,
    "0000 0100 000": 12,
    "0000 0011 110": 13,
    "0000 0011 100": 14,
    "0000 0011 010": 15,
    "0000 0011 000": 16
};

// Spec table 2-B.5. Values are the DC size.
export const dctDcSizeLuminanceCodes = {
    "100": 0,
    "00": 1,
    "01": 2,
    "101": 3,
    "110": 4,
    "1110": 5,
    "1111 0": 6,
    "1111 10": 7,
    "1111 110": 8
};

export const dctDcSizeChrominanceCodes = {
    "00": 0,
    "01": 1,
    "10": 2,
    "110": 3,
    "1110": 4,
    "1111 0": 5,
    "1111 10": 6,
    "1111 110": 7,
    "1111 1110": 8
};

// Spec table 2-B.5. Values are "run,level". Codes leave out the sign bit that follows them.
const dctCoeffCommonCodes = {
    "0000 01": "escape",
    "011": "1,1",
    "0100": "0,2",
    "0101": "2,1",
    "0010 1": "0,3",
    "0011 1": "3,1",
    "0011 0": "4,1",
    "0001 10": "1,2",
    "0001 11": "5,1",
    "0001 01": "6,1",
    "0001 00": "7,1",
    "0000 110": "0,4",
    "0000 100": "2,2",
    "0000 111": "8,1",
    "0000 101": "9,1",
    "0010 0110": "0,5",
    "0010 0001": "0,6",
    "0010 0101": "1,3",
    "0010 0100": "3,2",
    "0010 0111": "10,1",
    "0010 0011": "11,1",
    "0010 0010": "12,1",
    "0010 0000": "13,1",
    "0000 0010 10": "0,7",
    "0000 0011 00": "1,4",
    "0000 0010 11": "2,3",
    "0000 0011 11": "4,2",
    "0000 0010 01": "5,2",
    "0000 0011 10": "14,1",
    "0000 0011 01": "15,1",
    "0000 0010 00": "16,1",
    "0000 0001 1101": "0,8",
    "0000 0001 1000": "0,9",
    "0000 0001 0011": "0,10",
    "0000 0001 0000": "0,11",
    "0000 0001 1011": "1,5",
    "0000 0001 0100": "2,4",
    "0000 0001 1100": "3,3",
    "0000 0001 0010": "4,3",
    "0000 0001 1110": "6,2",
    "0000 0001 0101": "7,2",
    "0000 0001 0001": "8,2",
    "0000 0001 1111": "17,1",
    "0000 0001 1010": "18,1",
    "0000 0001 1001": "19,1",
    "0000 0001 0111": "20,1",
    "0000 0001 0110": "21,1",
    "0000 0000 1101 0": "0,12",
    "0000 0000 1100 1": "0,13",
    "0000 0000 1100 0": "0,14",
    "0000 0000 1011 1": "0,15",
    "0000 0000 1011 0": "1,6",
    "0000 0000 1010 1": "1,7",
    "0000 0000 1010 0": "2,5",
    "0000 0000 1001 1": "3,4",
    "0000 0000 1001 0": "5,3",
    "0000 0000 1000 1": "9,2",
    "0000 0000 1000 0": "10,2",
    "0000 0000 1111 1": "22,1",
    "0000 0000 1111 0": "23,1",
    "0000 0000 1110 1": "24,1",
    "0000 0000 1110 0": "25,1",
    "0000 0000 1101 1": "26,1",
    "0000 0000 0111 11": "0,16",
    "0000 0000 0111 10": "0,17",
    "0000 0000 0111 01": "0,18",
    "0000 0000 0111 00": "0,19",
    "0000 0000 0110 11": "0,20",
    "0000 0000 0110 10": "0,21",
    "0000 0000 0110 01": "0,22",
    "0000 0000 0110 00": "0,23",
    "0000 0000 0101 11": "0,24",
    "0000 0000 0101 10": "0,25",
    "0000 0000 0101 01": "0,26",
    "0000 0000 0101 00": "0,27",
    "0000 0000 0100 11": "0,28",
    "0000 0000 0100 10": "0,29",
    "0000 0000 0100 01": "0,30",
    "0000 0000 0100 00": "0,31",
    "0000 0000 0011 000": "0,32",
    "0000 0000 0010 111": "0,33",
    "0000 0000 0010 110": "0,34",
    "0000 0000 0010 101": "0,35",
    "0000 0000 0010 100": "0,36",
    "0000 0000 0010 011": "0,37",
    "0000 0000 0010 010": "0,38",
    "0000 0000 0010 001": "0,39",
    "0000 0000 0010 000": "0,40",
    "0000 0000 0011 111": "1,8",
    "0000 0000 0011 110": "1,9",
    "0000 0000 0011 101": "1,10",
    "0000 0000 0011 100": "1,11",
    "0000 0000 0011 011": "1,12",
    "0000 0000 0011 010": "1,13",
    "0000 0000 0011 001": "1,14",
    "0000 0000 0001 0011": "1,15",
    "0000 0000 0001 0010": "1,16",
    "0000 0000 0001 0001": "1,17",
    "0000 0000 0001 0000": "1,18",
    "0000 0000 0001 0100": "6,3",
    "0000 0000 0001 1010": "11,2",
    "0000 0000 0001 1001": "12,2",
    "0000 0000 0001 1000": "13,2",
    "0000 0000 0001 0111": "14,2",
    "0000 0000 0001 0110": "15,2",
    "0000 0000 0001 0101": "16,2",
    "0000 0000 0001 1111": "27,1",
    "0000 0000 0001 1110": "28,1",
    "0000 0000 0001 1101": "29,1",
    "0000 0000 0001 1100": "30,1",
    "0000 0000 0001 1011": "31,1"
};

// The first coefficient of a non-intra block can't be end_of_block, so run 0, level 1 has a shorter code
export const dctCoeffFirstCodes = {
    ...dctCoeffCommonCodes,
    "1": "0,1"
};

export const dctCoeffNextCodes = {
    ...dctCoeffCommonCodes,
    "10": "end_of_block",
    "11": "0,1"
};

export const allTables: Record<string, Record<string, VlcValue>> = {
    macroblockAddressIncrement: macroblockAddressIncrementCodes,
    macroblockTypeI: macroblockTypeICodes,
    macroblockTypeP: macroblockTypePCodes,
    macroblockTypeB: macroblockTypeBCodes,
    macroblockTypeD: macroblockTypeDCodes,
    macroblockPatttern: macroblockPattternCodes,
    motionCode: motionCodes,
    dctDcSizeLuminance: dctDcSizeLuminanceCodes,
    dctDcSizeChrominance: dctDcSizeChrominanceCodes,
    dctCoeffFirst: dctCoeffFirstCodes,
    dctCoeffNext: dctCoeffNextCodes
};

for (const name in allTables) {
    new VlcTable(name, allTables[name]).print();
}
