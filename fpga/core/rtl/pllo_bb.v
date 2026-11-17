// Blackbox declaration of the GW1N-2 PLLO for Yosys (ports and parameters only).
(* blackbox *)
module PLLO (
    input CLKIN, input CLKFB, input RESET, input RESET_P, input RESET_I, input RESET_S,
    input [5:0] FBDSEL, input [5:0] IDSEL,
    input [6:0] ODSELA, input [6:0] ODSELB, input [6:0] ODSELC, input [6:0] ODSELD,
    input [3:0] DTA, input [3:0] DTB, input [4:0] ICPSEL, input [2:0] LPFRES,
    input [1:0] PSSEL, input PSDIR, input PSPULSE,
    input ENCLKA, input ENCLKB, input ENCLKC, input ENCLKD,
    output LOCK, output CLKOUTA, output CLKOUTB, output CLKOUTC, output CLKOUTD);
parameter FCLKIN = "100.0";
parameter DYN_IDIV_SEL = "FALSE";
parameter IDIV_SEL = 0;
parameter DYN_FBDIV_SEL = "FALSE";
parameter FBDIV_SEL = 0;
parameter DYN_ODIVA_SEL = "FALSE";
parameter ODIVA_SEL = 6;
parameter DYN_ODIVB_SEL = "FALSE";
parameter ODIVB_SEL = 6;
parameter DYN_ODIVC_SEL = "FALSE";
parameter ODIVC_SEL = 6;
parameter DYN_ODIVD_SEL = "FALSE";
parameter ODIVD_SEL = 6;
parameter CLKOUTA_EN = "TRUE";
parameter CLKOUTB_EN = "TRUE";
parameter CLKOUTC_EN = "TRUE";
parameter CLKOUTD_EN = "TRUE";
parameter DYN_DTA_SEL = "FALSE";
parameter DYN_DTB_SEL = "FALSE";
parameter CLKOUTA_DT_DIR = 1'b1;
parameter CLKOUTB_DT_DIR = 1'b1;
parameter CLKOUTA_DT_STEP = 0;
parameter CLKOUTB_DT_STEP = 0;
parameter CLKA_IN_SEL = 2'b00;
parameter CLKA_OUT_SEL = 1'b0;
parameter CLKB_IN_SEL = 2'b00;
parameter CLKB_OUT_SEL = 1'b0;
parameter CLKC_IN_SEL = 2'b00;
parameter CLKC_OUT_SEL = 1'b0;
parameter CLKD_IN_SEL = 2'b00;
parameter CLKD_OUT_SEL = 1'b0;
parameter CLKFB_SEL = "INTERNAL";
parameter DYN_DPA_EN = "FALSE";
parameter DYN_PSB_SEL = "FALSE";
parameter DYN_PSC_SEL = "FALSE";
parameter DYN_PSD_SEL = "FALSE";
parameter PSB_COARSE = 1;
parameter PSB_FINE = 0;
parameter PSC_COARSE = 1;
parameter PSC_FINE = 0;
parameter PSD_COARSE = 1;
parameter PSD_FINE = 0;
parameter DTMS_ENB = "FALSE";
parameter DTMS_ENC = "FALSE";
parameter DTMS_END = "FALSE";
parameter RESET_I_EN = "FALSE";
parameter RESET_S_EN = "FALSE";
parameter DYN_ICP_SEL = "FALSE";
parameter ICP_SEL = 5'bXXXXX;
parameter DYN_RES_SEL = "FALSE";
parameter LPR_REF = 7'bXXXXXXX;
endmodule
