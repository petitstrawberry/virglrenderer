{ lib, stdenv, virglrenderer, shaderc, spirv-cross, src }:
virglrenderer.overrideAttrs (previous: {
  inherit src;
  buildInputs = (previous.buildInputs or [ ])
    ++ lib.optionals stdenv.isDarwin [ shaderc spirv-cross ];
  mesonFlags = (previous.mesonFlags or [ ]) ++ [
    "-Dmetal-compute=${if stdenv.isDarwin then "enabled" else "disabled"}"
  ];
  doCheck = true;
})
