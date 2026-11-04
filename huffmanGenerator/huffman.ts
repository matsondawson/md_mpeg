import { Uint8Buffer } from "./buffer";

export type VlcRow = {
    value: number;
    length: number;
    code: string;
};

export class VlcTable {
    maxBitLength: number;
    decodingTable: Array<VlcRow | undefined>;

    constructor(public readonly name: string, codes: Record<string, number>) {
        this.maxBitLength = Object.keys(codes).reduce((maxLength: number, code: string): number =>
            Math.max(maxLength, code.replace(/ /g, "").length), 0);
        this.decodingTable = new Array<VlcRow | undefined>(Math.pow(2, this.maxBitLength)).fill(undefined);

        if (this.maxBitLength > 14) throw Error("Bit length too long for naive VLC implementation");

        for (let code in codes) {
            const value = codes[code];
            code = code.replace(/ /g, "");
            const start = parseInt(code.padEnd(this.maxBitLength, "0"), 2);
            const end = parseInt(code.padEnd(this.maxBitLength, "1"), 2);
            for (let i = start; i <= end; i++) {
                this.decodingTable[i] = { code, value, length: code.length };
            }
        }

        // check table correctly generated
        // if (this.decodingTable.some((v) => v === undefined)) console.warn("VLC table incomplete");
        console.log(this);
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

new VlcTable("macroblockPatttern", macroblockPattternCodes);