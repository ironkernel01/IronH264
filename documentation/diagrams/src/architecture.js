// Run with node from the sibling IronForge workspace or pass its path as argv[2].
const fs = require("node:fs");
const path = require("node:path");
const forge = path.resolve(process.argv[2] || path.join(__dirname, "../../../../IronForge"));
const { newDeck, helpers } = require(path.join(forge, "documentation/drawings/lib/engslides.js"));

async function main() {
  const deck = newDeck("IronH264 Baseline encoder data path", "IronKernel");
  const { box, container, frame, label, notes, wire } = helpers(deck);
  const slide = deck.addSlide();
  frame(slide);

  container(slide, 2.55, 1.00, 8.28, 4.60, "IronH264 Constrained Baseline encoder");
  box(slide, 0.66, 2.00, 1.38, 0.85, "Source DDR", ["Packed YUYV", "Previous reference"]);
  box(slide, 2.90, 2.00, 1.77, 0.85, "Read DMA", ["Picture strips", "Reference samples"]);
  box(slide, 5.10, 2.00, 2.07, 0.85, "Prediction and coding", ["Intra or motion search", "Transform and quantize"]);
  box(slide, 7.92, 2.00, 2.35, 0.85, "CAVLC and NAL packer", ["Slice payloads", "Picture index records"]);
  box(slide, 11.34, 2.00, 1.31, 0.85, "Output DDR", ["Bitstream ring", "Index ring"]);
  box(slide, 5.10, 4.20, 2.07, 0.85, "Reconstruction", ["Inverse transform", "Add prediction"]);
  box(slide, 7.92, 4.20, 2.35, 0.85, "Reference writeback", ["Optional deblocking", "Reconstructed luma and chroma"]);
  box(slide, 11.34, 4.20, 1.31, 0.85, "Reference DDR", ["Next P picture"]);
  box(slide, 0.66, 4.20, 1.38, 0.85, "Host", ["Submit picture", "Consume access unit"]);
  box(slide, 2.90, 4.20, 1.77, 0.85, "Control and status", ["Picture parameters", "Ring pointers and IRQ"]);

  wire(slide, 2.04, 2.425, 2.90, 2.425);
  wire(slide, 4.67, 2.425, 5.10, 2.425);
  wire(slide, 7.17, 2.425, 7.92, 2.425);
  wire(slide, 10.27, 2.425, 11.34, 2.425);
  wire(slide, 6.135, 2.85, 6.135, 4.20);
  label(slide, 6.24, 3.30, 1.65, "Levels and prediction");
  wire(slide, 7.17, 4.625, 7.92, 4.625);
  wire(slide, 10.27, 4.625, 11.34, 4.625);
  wire(slide, 2.04, 4.625, 2.90, 4.625, { start: true, dash: "dash" });
  label(slide, 2.04, 4.39, 0.51, "AXI4-Lite", 7.5, "center");
  wire(slide, 3.785, 4.20, 3.785, 2.85, { dash: "dash" });

  notes(slide, 0.68, 5.98, 12.0, 0.95, [
    "DDR boxes identify logical buffer regions. The Zybo AXI wrapper accesses all regions through one 64-bit bus master.",
    "P pictures reference a previously reconstructed picture. The host alternates the reference and reconstruction buffers between pictures.",
    "Software supplies SPS and PPS and uses the picture index to locate the compressed access unit."
  ]);

  const assets = path.resolve(__dirname, "../../assets");
  fs.mkdirSync(assets, { recursive: true });
  await deck.writeFile({ fileName: path.join(assets, "architecture.pptx") });
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
