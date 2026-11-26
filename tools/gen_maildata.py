#!/usr/bin/env python3
"""Generate src/gui/mail_data.h — the fictional mailbox corpus used by the
hotmail preset UI. Accounts and messages are entirely invented: plausible
names, senders, subjects, amounts and dates, but every value is fabricated.
Fully offline: the header is compiled into the binary.

Usage: python3 tools/gen_maildata.py
"""
import json
import random

random.seed(20261125)

FIRST = ["Marie", "Lucas", "Emma", "Hugo", "Chloé", "Nathan", "Léa", "Théo",
         "Camille", "Louis", "Sarah", "Jules", "Manon", "Adam", "Inès", "Gabriel",
         "Laura", "Mehdi", "Elena", "Marco", "Sofia", "David", "Julia", "Kevin",
         "Amélie", "Ryan", "Yasmine", "Antoine"]
LAST = ["Bernard", "Dubois", "Martin", "Nguyen", "Petit", "Rossi", "Garcia", "Klein",
        "Moreau", "Lefèvre", "Silva", "Novak", "Costa", "Lambert", "Fontaine",
        "Schmitt", "Leroy", "Bianchi", "Ferrer", "Dupont", "Laurent", "Meyer"]
DOMAINS = ["hotmail.com", "hotmail.fr", "outlook.com", "outlook.fr", "live.fr", "live.com"]

BANK_SENDERS = [
    ("BNP Paribas", "alerte@notification.bnpparibas.fr"),
    ("Crédit Agricole", "securite@ca-alaerte.fr"),
    ("La Banque Postale", "notification@labanquepostale.fr"),
    ("Revolut", "no-reply@revolut.com"),
    ("PayPal", "service@paypal.fr"),
    ("Société Générale", "alerte@securite-sg.fr"),
]
SHOP_SENDERS = [
    ("Amazon.fr", "commande@amazon.fr"),
    ("Cdiscount", "serviceclient@cdiscount.com"),
    ("Fnac", "commande@fnac.com"),
    ("Vinted", "notifications@vinted.fr"),
    ("Decathlon", "commande@decathlon.com"),
    ("Sephora", "fidelite@sephora.fr"),
]
UTIL_SENDERS = [
    ("EDF", "votre-compte@edf.fr"),
    ("Orange", "facture@orange.fr"),
    ("SFR", "mon-compte@sfr.fr"),
    ("Impots.gouv.fr", "ne-pas-repondre@dgfip.finances.gouv.fr"),
    ("Ameli", "notification@ameli.fr"),
    ("Veolia", "eaux@veolia.com"),
]
TECH_SENDERS = [
    ("Netflix", "info@mailer.netflix.com"),
    ("Spotify", "no-reply@spotify.com"),
    ("Steam", "noreply@steampowered.com"),
    ("LinkedIn", "messages-noreply@linkedin.com"),
    ("Discord", "no-reply@discord.com"),
    ("Google", "no-reply@accounts.google.com"),
]
TRAVEL_SENDERS = [
    ("Booking.com", "customer.service@booking.com"),
    ("Airbnb", "automated@airbnb.com"),
    ("SNCF Connect", "billet@email.sncf.com"),
    ("Ryanair", "no-reply@ryanair.com"),
    ("Uber", "receipts@uber.com"),
    ("Deliveroo", "no-reply@deliveroo.fr"),
]

SUBJECTS = [
    ("security", "Alerte de connexion : nouvelle connexion détectée"),
    ("security", "Connexion inhabituelle à votre compte"),
    ("verify", "Votre code de vérification"),
    ("verify", "Confirmez votre adresse e-mail"),
    ("order", "Votre commande a été expédiée"),
    ("order", "Votre colis est en cours de livraison"),
    ("order", "Confirmation de commande"),
    ("bill", "Votre facture est disponible"),
    ("bill", "Prélèvement programmé pour le {day} {month}"),
    ("travel", "Votre réservation est confirmée"),
    ("travel", "Votre billet électronique"),
    ("news", "Vos nouveautés du mois"),
    ("news", "Ne manquez pas nos offres de la semaine"),
    ("sub", "Votre abonnement a été renouvelé"),
]

MONTHS = ["janvier", "février", "mars", "avril", "mai", "juin", "juillet",
          "août", "septembre", "octobre", "novembre", "décembre"]

BODIES = {
    "security": """Bonjour {first},

Une nouvelle connexion à votre compte a été détectée.

Appareil : Windows 11 · Chrome
Localisation : Lyon, France
Date : {date} à {time}
Adresse IP : 92.184.{ip3}.{ip4}

Si vous êtes à l'origine de cette connexion, aucune action n'est requise.
Dans le cas contraire, modifiez immédiatement votre mot de passe et vérifiez
les appareils connectés depuis les paramètres de sécurité.

L'équipe {sender}""",
    "verify": """Bonjour,

Voici votre code de vérification : {code}

Ce code expire dans 10 minutes. Pour votre sécurité, ne le partagez
avec personne — même avec un conseiller {sender}.

Si vous n'avez pas demandé ce code, vous pouvez ignorer cet e-mail.

{sender}""",
    "order": """Bonjour {first},

Nous vous confirmons l'expédition de votre commande.

Numéro de commande : {order}
Transporteur : Colissimo
Livraison estimée : {date}

Suivez votre colis à tout moment depuis votre espace client.

Merci pour votre confiance,
L'équipe {sender}""",
    "bill": """Bonjour {first},

Votre facture {ref} est disponible dans votre espace client.

Montant à régler : {amount} € TTC
Échéance : {date}

Pour toute question, vous pouvez nous contacter via votre espace personnel.

Le Service Clients {sender}""",
    "travel": """Bonjour {first},

Votre réservation est confirmée.

Référence : {ref}
Date : {date}
Montant total : {amount} €

Vous trouverez votre billet ainsi que toutes les informations pratiques
en pièce jointe de cet e-mail. Merci de la présenter à l'embarquement.

Bon voyage !
{sender}""",
    "news": """Bonjour {first},

Découvrez les nouveautés sélectionnées pour vous ce mois-ci : contenus,
offres et recommandations personnalisées selon vos usages.

À très vite,
L'équipe {sender}""",
    "sub": """Bonjour {first},

Votre abonnement {ref} a été renouvelé avec succès pour un montant de
{amount} €. Le prélèvement apparaîtra sous 2 à 3 jours ouvrés.

Vous pouvez gérer votre abonnement à tout moment depuis votre compte.

L'équipe {sender}""",
}

SENDER_GROUPS = [BANK_SENDERS, SHOP_SENDERS, UTIL_SENDERS, TECH_SENDERS, TRAVEL_SENDERS]
GROUP_KIND = {"security": "all", "verify": "all", "order": SHOP_SENDERS,
              "bill": UTIL_SENDERS + BANK_SENDERS, "travel": TRAVEL_SENDERS,
              "news": TECH_SENDERS, "sub": TECH_SENDERS}


def passwd():
    styles = [
        lambda: f"{random.choice(['Marie','Kevin','Laura','Mehdi','Sofia','Hugo'])}{random.randint(1970,2005)}{random.choice(['!','#','@',''])}",
        lambda: f"{random.choice(['Bleu','Violet','Rouge','Zen','Nova','Eclair'])}_{random.randint(10,99)}",
        lambda: f"{random.choice(['foot','chat','lune','soleil','mer','foret'])}{random.randint(100,999)}!",
        lambda: f"{random.choice('ABCDEFGH')}{random.choice('abcdefgh')}{random.randint(100000,999999)}",
    ]
    return random.choice(styles)()


def gen_body(kind, sender, first, date, time, ip):
    tpl = BODIES[kind]
    return (tpl
            .replace("{first}", first)
            .replace("{sender}", sender)
            .replace("{date}", date)
            .replace("{time}", time)
            .replace("{ip3}", str(random.randint(10, 250)))
            .replace("{ip4}", str(random.randint(2, 250)))
            .replace("{code}", f"{random.randint(100000, 999999)}")
            .replace("{order}", f"40{random.randint(2000000, 9999999)}-{random.randint(1000000, 9999999)}")
            .replace("{ref}", f"{random.choice('ABCDEFGHJKMNPQRSTUVWXYZ')}{random.randint(1000000, 9999999)}")
            .replace("{amount}", f"{random.randint(9, 189)}.{random.choice(['90','99','50','00'])}")
            .replace("{day}", str(random.randint(1, 28)))
            .replace("{month}", random.choice(MONTHS)))


accounts = []
messages = []
used = set()
for i in range(24):
    while True:
        fn, ln = random.choice(FIRST), random.choice(LAST)
        pattern = random.choice(["{f}.{l}", "{l}.{f}", "{f}{l}", "{f}_{l}{n}", "{f}{n}"])
        local = pattern.format(f=fn.lower(), l=ln.lower(), n=random.randint(1, 99))
        local = local.replace("é", "e").replace("è", "e")
        addr = f"{local}@{random.choice(DOMAINS)}"
        if addr not in used:
            used.add(addr)
            break
    n_msgs = random.randint(4, 7)
    kinds = random.sample(list(BODIES.keys()), min(3, len(BODIES.keys())))
    while len(kinds) < n_msgs:
        kinds.append(random.choice(list(BODIES.keys())))
    random.shuffle(kinds)
    first = 0
    for j, kind in enumerate(kinds):
        pool = GROUP_KIND.get(kind)
        if pool == "all" or pool is None:
            pool = random.choice(SENDER_GROUPS)
        sender, saddr = random.choice(pool)
        day = random.randint(3, 24)
        month = random.choice(MONTHS[:11])
        date = f"{day} {month} 2026"
        time = f"{random.randint(7,22)}:{random.randint(10,59)}"
        subj = dict(SUBJECTS).get(kind, SUBJECTS[0][1])
        subj = subj.replace("{day}", str(day)).replace("{month}", month)
        if kind == "news" and random.random() < 0.5:
            subj = random.choice([s for k, s in SUBJECTS if k == "news"])
        messages.append({
            "from_name": sender, "from_addr": saddr, "subject": subj,
            "date": f"{day} {month}", "time": time,
            "body": gen_body(kind, sender, fn, date, time, 0),
        })
    accounts.append({"addr": addr, "pass": passwd(), "first": first, "count": n_msgs})

# fix first indices (messages were appended flat per account, so track spans)
pos = 0
for acc in accounts:
    acc["first"] = pos
    pos += acc["count"]

# ---------------- render header ----------------
def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


out = []
out.append('// GENERATED by tools/gen_maildata.py — fictional mailbox corpus for the')
out.append('// mailbox preset UI. Every account, sender, amount and date below is')
out.append('// invented; nothing here touches a real service.')
out.append('#pragma once')
out.append('')
out.append('namespace mailbox {')
out.append('')
out.append('struct Account { const char* addr; const char* pass; int first; int count; };')
out.append('struct Message { const char* from_name; const char* from_addr; const char* subject;')
out.append('                 const char* date; const char* time; const char* body; };')
out.append('')
out.append('inline const Message kMessages[] = {')
for m in messages:
    out.append('{%s, %s, %s, %s, %s,\n %s},' % (
        cstr(m["from_name"]), cstr(m["from_addr"]), cstr(m["subject"]),
        cstr(m["date"]), cstr(m["time"]), cstr(m["body"])))
out.append('};')
out.append('')
out.append('inline const Account kAccounts[] = {')
for a in accounts:
    out.append('{%s, %s, %d, %d},' % (cstr(a["addr"]), cstr(a["pass"]), a["first"], a["count"]))
out.append('};')
out.append('')
out.append('inline const int kAccountCount = %d;' % len(accounts))
out.append('inline const int kMessageCount = %d;' % len(messages))
out.append('')
out.append('} // namespace mailbox')
out.append('')

with open("src/gui/mail_data.h", "w", encoding="utf-8") as f:
    f.write("\n".join(out))

print(f"wrote src/gui/mail_data.h — {len(accounts)} accounts, {len(messages)} messages")
for a in accounts[:3]:
    print("  sample:", a["addr"], "/", a["pass"], f"({a['count']} msgs)")
