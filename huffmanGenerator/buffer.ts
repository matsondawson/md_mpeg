export class Uint8Buffer {
  byteOffset = 0;
  bitBufferRemaining = 0;
  bitBuffer = 0;

  constructor(private readonly data: Uint8Array) {}

  bufferEmpty(): boolean {
    return this.byteOffset == this.data.length && this.bitBufferRemaining == 0;
  }

  /**
   *
   */
  nextBits(): boolean {
    if (this.byteOffset == this.data.length) return false;
    this.bitBuffer = (this.bitBuffer << 8) + this.data[this.byteOffset++];
    this.bitBufferRemaining += 8;
    return true;
  }

  /**
   *
   * @param bitCount
   * @param padBits
   */
  peek(bitCount: number): number {
    const _byteOffset = this.byteOffset;
    const _bitBufferRemaining = this.bitBufferRemaining;
    const _bitBuffer = this.bitBuffer;

    while (this.bitBufferRemaining < bitCount && this.nextBits());
    // pad bits
    if (this.bitBufferRemaining < bitCount) {
      this.bitBufferRemaining <<= bitCount - this.bitBufferRemaining;
      this.bitBufferRemaining += bitCount;
    }
    const s = this.bitBuffer >> (this.bitBufferRemaining - bitCount);

    this.byteOffset = _byteOffset;
    this.bitBufferRemaining = _bitBufferRemaining;
    this.bitBuffer = _bitBuffer;

    return s;
  }

  /**
   * skip through array.
   *
   * @param bitCount How many bits to seek.
   * @return true for more data, false for end of data.
   */
  skip(bitCount: number): void {
    while (this.bitBufferRemaining < bitCount) {
      if (!this.nextBits()) throw Error("EOF");
    }
    // Knock the top bitCount bits off the buffer
    const mask = (1 << bitCount) - 1;
    this.bitBuffer &= ~(mask << (this.bitBufferRemaining - bitCount));
    this.bitBufferRemaining -= bitCount;
  }
}

/*
{
  console.log("a");
  const a = new Uint8Buffer(new Uint8Array([1, 2, 3]));
  for (let i = 0; i < 3; i++) {
    console.log(a.peek(8));
    a.skip(8);
    console.log("be", a.bufferEmpty());
  }
}

{
  console.log("b");
  const b = new Uint8Buffer(new Uint8Array([1, 2, 3]));
  for (let i = 0; i < 9; i++) {
    console.log(b.peek(8, 0));
    b.skip(1);
  }
}
*/
