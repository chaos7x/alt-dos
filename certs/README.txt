CA-Liste fuer TLS (WGET https://), auf der Platte als \DOS\SYSTEM\CACERT.PEM

Herkunft: https://curl.se/ca/cacert.pem (vom curl-Projekt aus den
Zertifikaten von Mozilla - certdata.txt in NSS - erzeugt; Stand siehe Kopf
der Datei, "Certificate data from Mozilla as of"). Lizenz: Mozilla Public
License 2.0 (MPL-2.0), https://www.mozilla.org/MPL/2.0/ - die Datei liegt
als eigenes Werk unveraendert neben AT-DOS (GPL-2.0-or-later), sie wird
nicht in Programme eingebaut.

Erneuern: Datei von der Adresse oben holen und hier ersetzen. Eigene
Zertifizierungsstellen (Firmennetz, Testserver) haengt man in PEM-Form an
\DOS\SYSTEM\CACERT.PEM an oder gibt WGET --ca-certificate=Datei mit.
