
import sys
from datetime import datetime
import yaml
import yaml_include
from moment import moment
from math import ceil
import os
import json

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

# End YAML parser

SOCRATES_ERASMUS_CODES = """
03 Sztuka i projektowanie
03.1 Sztuki piękne (malarstwo, rzeźba, grafika)
03.2 Muzyka i muzykologia
03.3 Sztuka (wykonawstwo)
03.4 Fotografia, kinematografia
03.5 Projektowanie (projektowanie graficzne, projektowanie przemysłowe, moda)
03.6 Historia sztuki
03.7 Inne związane ze sztuką i projektowaniem
04 Zarządzanie
04.1 Zarządzanie i język zarządzania
04.2 Zarządzanie i technologia
04.3 Rachunkowość, finanse
04.4 Turystyka
04.5 Zarządzanie zasobami ludzkimi
04.6 Studia sekretarskie
04.7 Marketing
04.9 Inne nauki związane z zarządzaniem
05 Edukacja, kształcenie nauczycieli
05.0 Edukacja, studia dla nauczycieli
05.1 Kształcenie nauczycieli
05.2 Edukacja podstawowa
05.3 Edukacja drugiego stopnia
05.4 Edukacja zawodowa i techniczna
05.5 Edukacja dorosłych
05.6 Edukacja specjalna
05.7 Nauki pedagogiczne
05.8 Psychologia wychowawcza
05.9 Inne nauki związane z edukacją i kształceniem nauczycieli
07 Geografia, geologia
07.1 Geografia
07.2 Nauka o środowisku
07.3 Geologia
07.4 Gleboznawstwo, hydrologia
07.6 Geodezja, kartografia, teledetekcja
07.7 Meteorologia
07.9 Inne nauki związane z geografią i geologią
08 Nauki humanistyczne
08.1 Filozofia
08.2 Teologia
08.3 Historia
08.4 Archeologia
08.9 Inne nauki humanistyczne
09 Filologia
09.1 Języki nowożytne Unii Europejskiej
09.2 Literatura powszechna i porównawcza
09.3 Językoznawstwo
09.4 Translatoryka
09.5 Filologia klasyczna
09.6 Języki spoza Unii Europejskiej
09.8 Języki rzadziej nauczane
09.9 Inne języki i nauki filologiczne
10 Prawo
10.1 Prawo porównawcze, język prawniczy
10.2 Prawo międzynarodowe
10.3 Prawo cywilne
10.4 Prawo karne. kryminologia
10.5 Prawo konstytucyjne
10.6 Administracja publiczna
10.7 Prawo Unii Europejskiej
10.8 Inne nauki prawne
11 Matematyka, Informatyka
11.1 Matematyka
11.2 Statystyka
11.3 Informatics, Computer Science # Informatyka
11.4 Sztuczna inteligencja
11.5 Nauki aktuarialne
11.9 Inne nauki matematyczne i informatyczne
12 Nauki medyczne
12.1 Medycyna
12.2 Psychiatria, psychologia kliniczna
12.3 Stomatologia
12.4 Weterynaria
12.5 Farmacja
12.6 Pielęgniarstwo, położnictwo, fizykoterapia
12.7 Zdrowie publiczne
12.8 Technologia medyczna
12.9 Inne nauki medyczne
13 Nauki przyrodnicze
13.1 Biologia
13.2 Fizyka
13.3 Chemia
13.4 Mikrobiologia, biotechnologia
13.5 Fizyka jądrowa, fizyka wysokich energii
13.6 Biochemia
13.7 Astronomia, astrofizyka
13.8 Oceanografia
13.9 Inne nauki przyrodnicze
14 Nauki społeczne
14.1 Nauki polityczne (politologia)
14.2 Socjologia
14.3 Ekonomia
14.4 Psychologia i nauki o zachowaniu
14.5 Pomoc społeczna (kształcenie pracowników socjalnych)
14.6 Stosunki międzynarodowe, europeistyka, studia regionalne
14.7 Antropologia
14.8 Studia nad rozwojem
14.9 Inne nauki społeczne
15 Komunikacja i informacja
15.1 Dziennikarstwo
15.2 Radio i TV
15.3 Public relations, reklama, promocja
15.4 Bibliologia
15.5 Dokumentacja, archiwa
15.6 Konserwacja zbiorów, muzealnictwo
15.9 Inne nauki związane z komunikacją i informacją
16 Inne dziedziny studiów
16.1 Wychowanie fizyczne, nauki związane ze sportem
16.2 Turyzm
16.3 Ekonomika w gospodarstwie domowym, odżywianie
16.4 Nawigacja
16.9 Inne dziedziny studiów
"""

SUPPORTED_LANGS = """
abkhazian (ab)
afar (aa)
afrikaans (af)
aghem (agq)
akan (ak)
akkadian (akk)
albanian (sq)
amharic (am)
ancientegyptian (egy)
ancientgreek (grc)
arabic (ar)
arabic-algeria (ar-DZ)
arabic-dz (ar-DZ)
arabic-egypt (ar-EG)
arabic-eg (ar-EG)
arabic-iraq (ar-IQ)
arabic-iq (ar-IQ)
arabic-jordan (ar-JO)
arabic-jo (ar-JO)
arabic-lebanon (ar-LB)
arabic-lb (ar-LB)
arabic-morocco (ar-MA)
arabic-ma (ar-MA)
arabic-palestinianterritories (ar-PS)
arabic-ps (ar-PS)
arabic-saudiarabia (ar-SA)
arabic-sa (ar-SA)
arabic-syria (ar-SY)
arabic-sy (ar-SY)
arabic-tunisia (ar-TN)
arabic-tn (ar-TN)
aramaic (arc)
aramaic-nabataean (arc-nbat)
aramaic-nbat (arc-nbat)
aramaic-palmyrene (arc-palm)
aramaic-palm (arc-palm)
armenian (hy)
assamese (as)
asturian (ast)
asu (asa)
atsam (cch)
avestan (ae)
awadhi (awa)
aymara (ay)
azerbaijani (az)
azerbaijani-cyrillic (az-Cyrl)
azerbaijani-cyrl (az-Cyrl)
azerbaijani-latin (az-Latn)
azerbaijani-latn (az-Latn)
bafia (ksf )
balinese (ban)
baluchi (bal)
bambara (bm)
bangla (bn)
bengali (bn)
basaa (bas)
bashkir (ba)
basque (eu)
bataktoba (bbc)
bavarian (bar)
belarusian (be)
bemba (bem)
bena (bez)
bhojpuri (bho)
blin (byn)
bodo (brx)
bosnian (bs)
bosnian-cyrillic (bs-Cyrl)
bosnian-cyrl (bs-Cyrl)
bosnian-latin (bs-Latn)
bosnian-latn (bs-Latn)
breton (br)
bulgarian (bg)
buriat (bua)
burmese (my)
cantonese (yue)
catalan (ca)
cebuano (ceb)
centralatlastamazight (tzm)
centralkurdish (ckb)
sorani (ckb)
centralkurdish-latin (ckb-Latn)
centralkurdish-latn (ckb-Latn)
chakma (ccp)
chechen (ce)
cherokee (chr)
chiga (cgg)
chinese (zh)
chinese-simplified (zh-Hans)
chinese-hans (zh-Hans)
chinese-traditional (zh-Hant)
chinese-hant (zh-Hant)
chinese-simplifiedhongkongsarchina (zh-Hans-HK)
chinese-hans-hk (zh-Hans-HK)
chinese-simplifiedmacausarchina (zh-Hans-MO)
chinese-hans-mo (zh-Hans-MO)
chinese-simplified-singapore (zh-Hans-SG)
chinese-hans-sg (zh-Hans-SG)
chinese-hant-hk (zh-Hant-HK)
chinese-traditionalhongkongsarchina (zh-Hant-HK)
chinese-hant-mo (zh-Hant-MO)
chinese-traditionalmacausarchina (zh-Hant-MO)
churchslavic (cu)
churchslavic-cyrs (cu-Cyrs)
churchslavic-glag (cu-Glag)
churchslavic-glagolitic (cu-Glag)
churchslavic-oldcyrillic (cu-Cyrs)
chuvash (cv)
classicalmandaic (myz)
colognian (ksh)
coptic (cop)
cornish (kw)
corsican (co)
croatian (hr)
czech (cs)
danish (da)
divehi (dv)
dogri (doi)
duala (dua)
dutch (nl)
dzongkha (dz)
egyptianarabic (arz)
embu (ebu)
english (en)
american (en-US)
americanenglish (en-US)
australian (en-AU)
australianenglish (en-AU)
british (en-GB)
britishenglish (en-GB)
canadian (en-CA)
canadianenglish (en-CA)
english-australia (en-AU)
english-au (en-AU)
english-canada (en-CA)
english-ca (en-CA)
english-unitedkingdom (en-GB)
english-gb (en-GB)
english-newzealand (en-NZ)
english-unitedstates (en-US)
english-nz (en-NZ)
english-us (en-US)
erzya (myv)
esperanto (eo)
estonian (et)
ewe (ee)
ewondo (ewo)
faroese (fo)
filipino (fil)
finnish (fi)
french (fr)
acadian (fr-x-acadian)
canadianfrench (fr-CA)
swissfrench (fr-CH)
french-belgium (fr-BE)
french-be (fr-BE)
french-canada (fr-CA)
french-ca (fr-CA)
french-luxembourg (fr-LU)
french-lu (fr-LU)
french-switzerland (fr-CH)
french-ch (fr-CH)
friulian (fur)
fulah (ff)
ga (gaa)
galician (gl)
ganda (lg)
geez (gez)
georgian (ka)
german (de)
german-traditional (de-1901)
austrian (de-AT)
german-austria (de-AT)
german-at (de-AT)
german-austria-traditional (de-AT-1901)
swisshighgerman (de-CH)
german-switzerland (de-CH)
german-ch (de-CH)
german-switzerlandtraditional (de-CH-1901)
gothic (got)
greek (el)
monotonicgreek (el)
polytonicgreek (el-polyton)
guarani (gn)
gujarati (gu)
gusii (guz)
haryanvi (bgc)
hausa (ha)
hausa-ghana (ha-GH)
hausa-gh (ha-GH)
hausa-niger (ha-NE)
hausa-ne (ha-NE)
hawaiian (haw)
hebrew (he)
hindi (hi)
hmongnjua (hnj)
hungarian (hu)
icelandic (is)
igbo (ig)
inarisami (smn)
indonesian (id)
ingush (inh)
interlingua (ia)
inuktitut (iu)
irish (ga)
italian (it)
japanese (ja)
javanese (jv)
jju (kaj)
jolafonyi (dyo)
kabuverdianu (kea)
kabyle (kab)
kaingang (kgp)
kako (kkj)
kalaallisut (kl)
kalenjin (kln)
kamba (kam)
kannada (kn)
kashmiri (ks)
kazakh (kk)
khmer (km)
kikuyu (ki)
kinyarwanda (rw)
komi (kv)
konkani (kok)
korean (ko)
korean-han (ko-Hani)
korean-hani (ko-Hani)
koyraborosenni (ses)
koyrachiini (khq)
kwasio (nmg)
kyrgyz (ky)
ladino (lad)
lakota (lkt)
langi (lag)
lao (lo)
latin (la)
ecclesiasticallatin (la-x-ecclesia)
classicallatin (la-x-classic)
medievallatin (la-x-medieval)
latvian (lv)
lepcha (lep)
ligurian (lij)
limbu (lif )
limbu-limb (lif-limb)
limbu-limbu (lif-limb)
lineara (lab)
lingala (ln)
lithuanian (lt)
lombard (lmo)
lowersorbian (dsb)
lowgerman (nds)
lu (khb)
lubakatanga (lu)
luo (luo)
luxembourgish (lb)
luyia (luy)
macedonian (mk)
machame (jmc)
magyaru l (hu)
maithili (mai)
makasar (mak)
makasar-bugi (mak-Bugi)
makasar-buginese (mak-Bugi)
makhuwameetto (mgh)
makonde (kde)
malagasy (mg)
malay (ms)
malay-brunei (ms-BN)
malay-bn (ms-BN)
malay-singapore (ms-SG)
malay-sg (ms-SG)
malayalam (ml)
maltese (mt)
manipuri (mni)
manx (gv)
maori (mi)
marathi (mr)
masai (mas)
mazanderani (mzn)
meru (mer)
meta (mgo)
mongolian (mn)
monotonicgreek (el)
morisyen (mfe)
mundang (mua)
muscogee (mus)
nama (naq)
navajo (nv)
nepali (ne)
newari (new)
newzealand (en-NZ)
ngiemboon (nnh)
ngomba (jgo)
nheengatu (yrl)
nigerianpidgin (pcm)
nko (nqo)
northernfrisian (frr)
northernkurdish (kmr)
kurmanji (kmr)
northernkurdish-arab (kmr-Arab)
northernkurdish-arabic (kmr-Arab)
northernluri (lrc)
northernsami (se)
samin (se)
northernsotho (nso)
northndebele (nd)
norwegian (no)
norsk (no)
nswissgerman (de-CH)
nuer (nus)
nyanja (ny)
nyankole (nyn)
nynorsk (nn)
norwegiannynorsk (nn)
occitan (oc)
odia (or)
oldnorse (non)
oromo (om)
ossetic (os)
papiamento (pap)
pashto (ps)
persian (fa)
farsi (fa)
phoenician (phn)
piedmontese (pms)
polish (pl)
portuguese (pt)
brazilian (pt-BR)
brazilianportuguese (pt-BR)
portuguese-brazil (pt-BR)
portuguese-br (pt-BR)
europeanportuguese (pt-PT)
portuguese-portugal (pt-PT)
portuguese-pt (pt-PT)
prussian (prg)
punjabi (pa)
punjabi-arabic (pa-Arab)
punjabi-arab (pa-Arab)
punjabi-gurmukhi (pa-Guru)
punjabi-guru (pa-Guru)
quechua (qu)
rajasthani (raj)
romanian (ro)
moldavian (ro-MD)
romanian-moldova (ro-MD)
romanian-md (ro-MD)
romansh (rm)
rombo (rof )
rundi (rn)
russian (ru)
rwa (rwk)
saho (ssy)
sakha (sah)
samaritan (smp)
samburu (saq)
sango (sg)
sangu (sbp)
sanskrit (sa)
sanskrit-bangla (sa-Beng)
sanskrit-beng (sa-Beng)
sanskrit-devanagari (sa-Deva)
sanskrit-deva (sa-Deva)
sanskrit-gujarati (sa-Gujr)
sanskrit-gujr (sa-Gujr)
sanskrit-kannada (sa-Knda)
sanskrit-knda (sa-Knda)
sanskrit-malayalam (sa-Mlym)
sanskrit-mlym (sa-Mlym)
sanskrit-telugu (sa-Telu)
sanskrit-telu (sa-Telu)
santali (sat)
saraiki (skr)
sardinian (sc)
scottishgaelic (gd)
sena (seh)
serbian (sr)
serbian-cyrillic (sr-Cyrl)
serbian-cyrl (sr-Cyrl)
serbian-cyrillicbosniaherzegovina (sr-Cyrl-BA)
serbian-cyrl-ba (sr-Cyrl-BA)
serbian-cyrillic-kosovo (sr-Cyrl-XK)
serbian-cyrl-xk (sr-Cyrl-XK)
serbian-cyrillic-montenegro (sr-Cyrl-ME)
serbian-cyrl-me (sr-Cyrl-ME)
serbian-latin (sr-Latn)
serbian-latn (sr-Latn)
serbian-latinbosniaherzegovina (sr-Latn-BA)
serbian-latn-ba (sr-Latn-BA)
serbian-latin-kosovo (sr-Latn-XK)
serbian-latn-xk (sr-Latn-XK)
serbian-latin-montenegro (sr-Latn-ME)
serbian-latn-me (sr-Latn-ME)
serbian-ijekavsk (sr-ijekavsk)
serbian-latn-ijekavsk (sr-Latn-ijekavsk)
shambala (ksb)
shona (sn)
sichuanyi (ii)
sicilian (scn)
silesian (szl)
sindhi (sd)
sindhi-devanagari (sd-deva)
sindhi-deva (sd-deva)
sindhi-khojki (sd-khoj)
sindhi-khoj (sd-khoj)
sindhi-khudawadi (sd-sind)
sindhi-sind (sd-sind)
sinhala (si)
sinteromani (rmo)
slovak (sk)
slovenian (sl)
slovene (sl)
soga (xog)
somali (so)
southernaltai (alt)
southernsotho (st)
southndebele (nr)
spanish (es)
mexican (es-MX)
mexicanspanish (es-MX)
spanish-mexico (es-MX)
spanish-mx (es-MX)
standardmoroccantamazight (zgh)
sundanese (su)
swahili (sw)
swati (ss)
swedish (sv)
swissgerman (gsw)
syriac (syr)
tachelhit (shi)
tachelhit-latin (shi-Latn)
tachelhit-latn (shi-Latn)
tachelhit-tifinagh (shi-Tfng)
tachelhit-tfng (shi-Tfng)
tainua (tdd)
taita (dav)
tajik (tg)
tamil (ta)
tangut (txg)
taroko (trv)
tasawaq (twq)
tatar (tt)
telugu (te)
teso (teo)
thai (th)
tibetan (bo)
tigre (tig)
tigrinya (ti)
tokpisin (tpi)
tongan (to)
tsonga (ts)
tswana (tn)
turkish (tr)
turkmen (tk)
tyap (kcg)
ukenglish (en-GB)
ukrainian (uk)
uppersorbian (hsb)
urdu (ur)
usenglish (en-US)
usorbian (hsb)
uyghur (ug)
uzbek (uz)
uzbek-arabic (uz-Arab)
uzbek-arab (uz-Arab)
uzbek-cyrillic (uz-Cyrl)
uzbek-cyrl (uz-Cyrl)
uzbek-latin (uz-Latn)
uzbek-latn (uz-Latn)
vai (vai)
vai-latin (vai-Latn)
vai-latn (vai-Latn)
vai-vai (vai-Vaii)
vai-vaii (vai-Vaii)
venda (ve)
vietnamese (vi)
volapuk (vo)
vunjo (vun)
walser (wae)
waray (war)
welsh (cy)
westernfrisian (fy)
wolaytta (wal)
wolof (wo)
xhosa (xh)
yangben (yav)
yiddish (yi)
yoruba (yo)
zarma (dje)
zulu (zu)
"""

def _yaml_partial(main_file_name):
    resource_base_path = os.path.dirname(main_file_name)
    def constructor(load, node):
        values = load.construct_mapping(node)
        path = values["path"]
        del values["path"]
        ref = (
            "".join(os.path.basename(path).lower().split(" "))
            .replace("_", "-")
            .strip()
        )
        values["ref"] = ref
        values["path"] = f"{path}.tex"
        full_path = os.path.join(resource_base_path, f"{path}.tex")
        if not os.path.isfile(full_path):
            raise Exception(f"Called !partial on path '{path}' that was resolved to {full_path} and the file does not exist")
        return values
    return constructor

def _yaml_soc_erasmus(load, node):
    text = str(node.value).strip()
    codes = { code: line for (code, line) in reversed([(line.strip().split(" ")[0], line) for line in [line.split("#")[0].strip() for line in SOCRATES_ERASMUS_CODES.split("\n")] if len(line.strip()) > 0]) }
    if text in codes:
        return dict(
            text=codes[text],
            code=text,
        )
    return dict(
        text=text,
        code="0",
    )

def _yaml_date(loader, node):
    text = str(node.value).strip()
    str_tokens = text.split("-")
    if len(str_tokens) == 0:
        text = ""
        diff_text = ""
    else:
        start = moment()
        end = start
        if len(str_tokens) == 1:
            start = moment(str_tokens[0])
            text = f"{start.format('DD-MM-YYYY')} - Now"
        else:
            start = moment(str_tokens[0])
            end = moment(str_tokens[1])
            text = f"{start.format('DD-MM-YYYY')} - {end.format('DD-MM-YYYY')}"

        days = (end-start)//(3600*24*1000)
        if days >= 30 and days <= 33:
            days = 32
        months = int(ceil(days / 32.0))
        diff_text = ""
        if months == 0:
            diff_text = f"{int(round(days))} days"
        elif months == 1:
            diff_text = "1 month"
        elif months < 12:
            diff_text = f"{months} months"
        else:
            yearPart = int((days / 32.0 / 12.0) * 10.0)
            if yearPart < 10:
                diff_text = f"{yearPart/10} year"
            elif yearPart == 10:
                diff_text = "1 year"
            else:
                diff_text = f"{yearPart/10} years"
    return dict(
        date=text,
        diff=diff_text,
    )

STRCMP_POLYFILL = """
% String comparison polyfill
  \\directlua{
    local write = tex.write
    function strcmp(A,B)
      if A == B then
        write("0")
      elseif A < B then
        write("-1")
     else
       write("1")
     end
    end
  }
  \\long\\def\\luaescapeexpandedstring#1{%
    \\luaescapestring{\\detokenize\\expandafter{\\expanded{#1}}}%
  } \\long\\def\\stringcompare#1#2{TT\\fi\\ifnum\\directlua{strcmp("\\luaescapeexpandedstring{#1}","\\luaescapeexpandedstring{#2}")}=0}
  \\providecommand\\dataeq[2]{TT\\fi\\edef\\v{\\expanded{\\data{#1}}}\\ifnum\\directlua{strcmp("\\luaescapeexpandedstring{\\data{#1}}","\\luaescapeexpandedstring{{#2}}")}=0}
"""

SPECIAL_VALUES = dict(
    CURRENT_DATE=datetime.today().strftime('%Y-%m-%d'),
)

def _merge(a: dict, b: dict, path=[]):
    for key in b:
        if key in a:
            if isinstance(a[key], dict) and isinstance(b[key], dict):
                _merge(a[key], b[key], path + [str(key)])
            elif a[key] != b[key]:
                # Conflict
                a[key] = b[key]
        else:
            a[key] = b[key]
    return a

def _replace_special_values_rec(data):
    if isinstance(data, dict):
        return {key: _replace_special_values_rec(value) for (key, value) in data.items()}
    elif isinstance(data, list):
        return [_replace_special_values_rec(item) for item in data]
    else:
        if isinstance(data, str):
            if data in SPECIAL_VALUES:
                return SPECIAL_VALUES[data]
        return data

def _replace_special_values(data):
    supported_langs_dict = {tokens[1].split(")")[0].strip(): tokens[0].strip() for tokens in reversed([line.strip().split("(") for line in SUPPORTED_LANGS.split("\n") if len(line.strip()) > 0])}
    new_data  = _replace_special_values_rec(data)
    if "languages" in new_data:
        new_data["languagesnames"] = []
        for lang in new_data["languages"]:
            lang_t = ""
            if lang in supported_langs_dict:
                lang_t = lang
            else:
                lang_tokens = lang.split("-")
                if len(lang_tokens) > 0:
                    if lang_tokens[0] in supported_langs_dict:
                        lang_t = lang_tokens[0]
                    elif lang_tokens[0].lower() in supported_langs_dict:
                        lang_t = lang_tokens[0].lower()
                    elif len(lang_tokens) > 1:
                        if lang_tokens[1].lower() in supported_langs_dict:
                            lang_t = lang_tokens[1].lower()
            if lang_t in supported_langs_dict:
                new_data["languagesnames"].append(supported_langs_dict[lang_t])
            else:
                new_data["languagesnames"].append(lang)
    else:
        new_data["languagesnames"] = []
    return new_data

# Define digit mapping
romanNumeralMap = (('M', 1000),
                   ('CM', 900),
                   ('D', 500),
                   ('CD', 400),
                   ('C', 100),
                   ('XC', 90),
                   ('L', 50),
                   ('XL', 40),
                   ('X', 10),
                   ('IX', 9),
                   ('V', 5),
                   ('IV', 4),
                   ('I', 1))

# _replacements = {
#     "&": r"\&",
#     "%": r"\%",
#     "$": r"\$",
#     "#": r"\#",
#     "_": r"\_",
#     "{": r"\{",
#     "}": r"\}",
#     "~": r"\textasciitilde{}",
#     "^": r"\^{}",
#     "\\": r"\textbackslash{}",
#     "\n": "\\newline%\n",
#     "-": r"{-}",
#     "\xA0": "~",  # Non-breaking space
#     "[": r"{[}",
#     "]": r"{]}",
# }
_replacements = {
    "&": r"\&",
    "%": r"\%",
    "$": r"\$",
    "#": r"\#",
    "_": r"\_",
    "{": r"\{",
    "}": r"\}",
    #"~": r"\textasciitilde{}",
    "^": r"\^{}",
    #"\\": r"\textbackslash{}",
    #"\n": "\\newline%\n",
    #"-": r"{-}",
    ",": ";",
    "\xA0": "~",  # Non-breaking space
    #"[": r"{[}",
    #"]": r"{]}",
}
_replacement_lookup = str.maketrans(_replacements)


def escape(string):
    """
    Replace special characters with their equivalent LaTeX macros.

    Args:
        string (str): The string to process.

    Returns:
        str: The string with special characters replaced with macros.
    """
    return string.translate(_replacement_lookup)

def toRoman(n):
    """convert integer to Roman numeral"""
    if not isinstance(n, int):
        raise Exception("decimals cannot be converted")
    if not (-1 < n < 5000):
        raise Exception("number out of range (must be 0..4999)")

    # special case
    if n == 0:
        return 'N'

    result = ""
    for numeral, integer in romanNumeralMap:
        while n >= integer:
            result += numeral
            n -= integer
    return result

def _extract_paths_rec(path, data):
    all_paths = dict()
    all_indices = []
    if isinstance(data, list):
        for (index, element) in enumerate(data):
            path_prefix = f"{path}.{index}"
            path_prefix_dot = f"{path}.."
            new_paths = list(_extract_paths_rec(path_prefix, element).items())
            new_paths_dot = [(path.removeprefix(path_prefix)+path_prefix_dot, value) for (path, value) in new_paths]
            new_paths = [*new_paths, *new_paths_dot]
            for (new_path, new_value) in new_paths:
                if new_path in all_paths:
                    if not isinstance(all_paths[new_path], list):
                        all_paths[new_path] = [all_paths[new_path]]
                    if isinstance(new_value, list):
                        all_paths[new_path] += new_value
                    else:
                        all_paths[new_path].append(new_value)
                else:
                    all_paths[new_path] = new_value
            all_indices.append(str(index))
        all_paths[path] = ",".join([escape(idx) for idx in all_indices])

    elif isinstance(data, dict):
        for (key, element) in data.items():
            new_paths = _extract_paths_rec(f"{path}.{key}", element).items()
            new_paths_dot = []# [(path.removeprefix(f"{path}.{key}")+f"{path}..", value) for (path, value) in new_paths]
            new_paths = [*new_paths, *new_paths_dot]
            for (new_path, new_value) in new_paths:
                if new_path in all_paths:
                    if not isinstance(all_paths[new_path], list):
                        all_paths[new_path] = [all_paths[new_path]]
                    if isinstance(new_value, list):
                        all_paths[new_path] += new_value
                    else:
                        all_paths[new_path].append(new_value)
                else:
                    all_paths[new_path] = new_value
            all_indices.append(str(key))
        all_paths[path] = ",".join([escape(idx) for idx in all_indices])
    else:
        value = str(data)
        all_paths[path] = value if path.endswith(".path") else escape(value)
    return all_paths

def _extract_paths(data):
    all_paths = dict()
    for (path, value) in _extract_paths_rec("", data).items():
        path_norm = path.removeprefix(".")
        if isinstance(value, list):
            all_paths[path_norm] = "{"+",".join(value)+"}"
        else:
            all_paths[path_norm] = "{"+value+"}"
    return [ (index, path, value) for (index, (path, value)) in enumerate(sorted(all_paths.items())) ]

def _convert(data):
    tex = [
        *[f"{line}\n" for line in STRCMP_POLYFILL.split("\n")],
        "\n",
        "\\makeatletter",
        "\\def\\data#1{",
    ]
    # Extract paths
    paths = _extract_paths(data)
    #for (_, path, value) in paths:
    #    print(f"{path} = {value}")

    #raise Exception("TROL")

    for (_, path, value) in paths:
        tex.append(f"\\if\\stringcompare{{#1}}{{{escape(path)}}}{value}\\else")
    tex.append("{??}")
    for _ in range(len(paths)):
        tex.append("\\fi")
    tex += [
        "}",
        "\\providecommand{\\foreachdata}[2]{",
        "\\if\\dataeq{#1}{??}\def\\v{{}}\else\def\\v{\data{#1}}\\fi",
        "\\expanded{\\noexpand\\foreach\\noexpand#2 in \\v}",
        "}",
        "\\def\\datax#1#2{\\if\\dataeq{#1}{??}{#2}\\else{\\data{#1}}\\fi}",
        "\\newcommand{\\datainput}[1]{\\edef\\valcap{\\data{#1}}\\edef\\uu{\\expandafter\\@firstofone\\valcap}\\input\\uu}",
        "\\newcommand{\\datapartial}[1]{\\edef\\valcap{\\data{#1.path}}\\edef\\uu{\\expandafter\\@firstofone\\valcap}\\input\\uu}",
        "\\newcommand{\\ddef}[2]{\edef\\valcap{\\data{#2}}\\edef#1{\\expandafter\\@firstofone\\valcap}}",
        "\\makeatother"
    ]
    return tex # [f"{line}" for line in tex]

def load_data(
    meta_file_name,
    main_file_name,
    vars_str_defs,
):
    yaml.add_constructor("!partial", _yaml_partial(main_file_name))
    yaml.add_constructor("!socrateserasmus", _yaml_soc_erasmus)
    yaml.add_constructor("!date", _yaml_date)
    yaml.add_constructor("!include", yaml_include.Constructor(base_dir=os.path.dirname(main_file_name)))

    defs = dict()
    with open(meta_file_name, 'r') as meta_file:
        defs = yaml.full_load(meta_file)

    for var_str in vars_str_defs:
        tokens = var_str.split("=")
        if len(tokens) > 1:
            [var_name, var_value, *_] = tokens
            defs[var_name] = var_value
    
    # Load main
    extracted_main_meta_lines = []
    with open(main_file_name, "r") as main_file:
        for line_raw in main_file:
            line = line_raw.replace("\n", "").replace("\r", "").strip()
            if line.startswith("%"):
                buf_line = line.removeprefix("%")
                # Potential nested comment
                buf_line = buf_line.split("%")[0]
                if len(buf_line.strip()) > 0:
                    buf_line_stripped = buf_line.strip().lower()
                    if not buf_line_stripped.startswith("!tex"):
                        extracted_main_meta_lines.append(buf_line)
            if len(line) > 0 and not line.startswith("%"):
                # Non-comment detected
                break
    if len(extracted_main_meta_lines) > 0:
        base_indent = len(extracted_main_meta_lines[0]) - len(extracted_main_meta_lines[0].lstrip())
        yaml_str = "\n".join([line[base_indent:] for line in extracted_main_meta_lines])
        #print(yaml_str)
        main_meta = yaml.full_load(yaml_str)
        defs = _merge(defs, main_meta)
        #print(defs)
        
    defs = _replace_special_values(defs)
    return defs

def create_vars_tex(
    vars_file_name,
    meta_file_name,
    main_file_name,
    vars_str_defs,
):
    defs = load_data(meta_file_name, main_file_name, vars_str_defs)
    eprint(f"[i] Output Latex config to: {vars_file_name}")    
    with open(vars_file_name, "w") as output_file:
        output_file.writelines([
            "%% Auto-generated variables\n",
            "\n",
            *_convert(defs),
            "\n",
            "%% End\n",
        ])

def create_vars_json(
    vars_file_name,
    meta_file_name,
    main_file_name,
    vars_str_defs,
):
    defs = load_data(meta_file_name, main_file_name, vars_str_defs)
    eprint(f"[i] Output json config to: {vars_file_name}")    
    with open(vars_file_name, "w") as output_file:
        output_file.write(json.dumps(defs, indent=4, sort_keys=True, default=str))

def create_ltex_json(
    ltex_json_file_name,
    meta_file_name,
    main_file_name,
    vars_str_defs,
):
    defs = load_data(meta_file_name, main_file_name, vars_str_defs)
    eprint(f"[i] Output LTex json config to: {ltex_json_file_name}")
    lang = "en-US"
    if "languages" in defs and defs["languages"]:
        lang = defs["languages"][0]
    with open(ltex_json_file_name, "w") as output_file:
        output_file.write(json.dumps({
            "ltex.enabled": True,
            "ltex.language": lang,
        }))
