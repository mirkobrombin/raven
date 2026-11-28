// The root certificates every Windows installation trusts for code signing that host certificate bundles leave out: the file name, where
// Microsoft publishes it and the SHA-256 of its DER bytes. Raven does not ship them: the runtime downloads the ones it lacks from these
// addresses the first time it checks a signature, and scripts/provision-roots.py reads the same table for builds and tests.
{"MicrosoftRootAuthority.crt", "http://www.microsoft.com/pki/certs/MicrosoftRootAuthority.crt", "f38406e540d7a9d90cb4a9479299640ffb6df9e224ecc7a01c0d9558d8dad77d"},
{"MicrosoftRootCert.crt", "http://www.microsoft.com/pki/certs/MicrosoftRootCert.crt", "885de64c340e3ea70658f01e1145f957fcda27aabeea1ab9faa9fdb0102d4077"},
{"MicRooCerAut_2010-06-23.crt", "http://www.microsoft.com/pki/certs/MicRooCerAut_2010-06-23.crt", "df545bf919a2439c36983b54cdfc903dfa4f37d3996d8d84b4c31eec6f3c163e"},
{"MicRooCerAut2011_2011_03_22.crt", "http://www.microsoft.com/pki/certs/MicRooCerAut2011_2011_03_22.crt", "847df6a78497943f27fc72eb93f9a637320a02b561d0a91b09e87a7807ed7c61"},
{"MicrosoftIdentityVerificationRootCA2020.crt", "http://www.microsoft.com/pkiops/certs/Microsoft%20Identity%20Verification%20Root%20Certificate%20Authority%202020.crt", "5367f20c7ade0e2bca790915056d086b720c33c1fa2a2661acf787e3292e1270"},
{"VeriSignClass3PublicPrimaryCA-G5.crt", "http://ctldl.windowsupdate.com/msdownload/update/v3/static/trustedr/en/4eb6d578499b1ccf5f581ead56be3d9b6744a5e5.crt", "9acfab7e43c8d880d06b262a94deeee4b4659989c3d0caf19baf6405e41ab7df"},
{"VeriSignUniversalRootCA.crt", "http://ctldl.windowsupdate.com/msdownload/update/v3/static/trustedr/en/3679ca35668772304d30a5fb873b0fa77bb70d54.crt", "2399561127a57125de8cefea610ddf2fa078b5c8067f4e828290bfb860e84b3c"},
